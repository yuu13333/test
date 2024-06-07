/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the plugins of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qxcbbackingstore.h"

#include "qxcbconnection.h"
#include "qxcbscreen.h"
#include "qxcbwindow.h"

#include <xcb/shm.h>
#include <xcb/xcb_image.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <stdio.h>
#include <errno.h>

#include <qdebug.h>
#include <qpainter.h>
#include <qscreen.h>
#include <QtGui/private/qhighdpiscaling_p.h>
#include <qpa/qplatformgraphicsbuffer.h>
#include <private/qimage_p.h>
#include <qendian.h>

#include <algorithm>
QT_BEGIN_NAMESPACE

class QXcbShmImage : public QXcbObject
{
public:
    QXcbShmImage(QXcbScreen *connection, const QSize &size, uint depth, QImage::Format format);
    ~QXcbShmImage() { destroy(true); }

    void resize(const QSize &size);

    void flushScrolledRegion(bool clientSideScroll);

    bool scroll(const QRegion &area, int dx, int dy);

    QImage *image() { return &m_qimage; }
    QPlatformGraphicsBuffer *graphicsBuffer() { return m_graphics_buffer; }

    QSize size() const { return m_qimage.size(); }

    bool hasAlpha() const { return m_hasAlpha; }
    bool hasShm() const { return m_shm_info.shmaddr != nullptr; }

    void put(xcb_drawable_t dst, const QRegion &region, const QPoint &offset);
    void preparePaint(const QRegion &region);

private:
    void doResize(const QSize &size, const xcb_format_t *fmt, QImage::Format format);

    void destroy(bool destroyShm);
    void doDestroyShm(bool detachFromXcb);

    void ensureGC(xcb_drawable_t dst);
    void shmPutImage(xcb_drawable_t drawable, const QRegion &region, const QPoint offset = QPoint());
    void flushPixmap(const QRegion &region, bool fullRegion = false);
    void setClip(const QRegion &region);

    xcb_window_t m_screen_root;

    xcb_shm_segment_info_t m_shm_info;
    qint32 m_segmentSize;

    xcb_image_t *m_xcb_image;

    QImage m_qimage;
    QPlatformGraphicsBuffer *m_graphics_buffer;

    xcb_gcontext_t m_gc;
    xcb_drawable_t m_gc_drawable;

    // When using shared memory these variables are used only for server-side scrolling.
    // When not using shared memory, we maintain a server-side pixmap with the backing
    // store as well as repainted content not yet flushed to the pixmap. We only flush
    // the regions we need and only when these are marked dirty. This way we can just
    // do a server-side copy on expose instead of sending the pixels every time
    xcb_pixmap_t m_xcb_pixmap;
    QRegion m_pendingFlush;

    // This is the scrolled region which is stored in server-side pixmap
    QRegion m_scrolledRegion;

    // When using shared memory this is the region currently shared with the server
    QRegion m_dirtyShm;

    // When not using shared memory this is a temporary buffer which is uploaded
    // as a pixmap region to server
    QByteArray m_flushBuffer;

    bool m_hasAlpha;
    bool m_clientSideScroll;
};

class QXcbShmGraphicsBuffer : public QPlatformGraphicsBuffer
{
public:
    QXcbShmGraphicsBuffer(QImage *image)
        : QPlatformGraphicsBuffer(image->size(), QImage::toPixelFormat(image->format()))
        , m_access_lock(QPlatformGraphicsBuffer::None)
        , m_image(image)
    { }

    bool doLock(AccessTypes access, const QRect &rect) override
    {
        Q_UNUSED(rect);
        if (access & ~(QPlatformGraphicsBuffer::SWReadAccess | QPlatformGraphicsBuffer::SWWriteAccess))
            return false;

        m_access_lock |= access;
        return true;
    }
    void doUnlock() override { m_access_lock = None; }

    const uchar *data() const override { return m_image->bits(); }
    uchar *data() override { return m_image->bits(); }
    int bytesPerLine() const override { return m_image->bytesPerLine(); }

    Origin origin() const override { return QPlatformGraphicsBuffer::OriginTopLeft; }
private:
    AccessTypes m_access_lock;
    QImage *m_image;
};

QXcbShmImage::QXcbShmImage(QXcbScreen *screen, const QSize &size, uint depth, QImage::Format format)
    : QXcbObject(screen->connection())
    , m_screen_root(screen->screen()->root)
    , m_segmentSize(0)
    , m_graphics_buffer(nullptr)
    , m_gc(0)
    , m_gc_drawable(0)
    , m_xcb_pixmap(0)
    , m_clientSideScroll(false)
{
    const xcb_format_t *fmt = connection()->formatForDepth(depth);
    Q_ASSERT(fmt);

    m_hasAlpha = QImage::toPixelFormat(format).alphaUsage() == QPixelFormat::UsesAlpha;
    if (!m_hasAlpha)
        format = qt_maybeAlphaVersionWithSameDepth(format);

    memset(&m_shm_info, 0, sizeof m_shm_info);

    doResize(size, fmt, format);
}

void QXcbShmImage::resize(const QSize &size)
{
    xcb_format_t fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.depth = m_xcb_image->depth;
    fmt.bits_per_pixel = m_xcb_image->bpp;
    fmt.scanline_pad = m_xcb_image->scanline_pad;
    destroy(false);
    doResize(size, &fmt, m_qimage.format());
}

void QXcbShmImage::doResize(const QSize &size, const xcb_format_t *fmt, QImage::Format format)
{
    m_xcb_image = xcb_image_create(size.width(), size.height(),
                                   XCB_IMAGE_FORMAT_Z_PIXMAP,
                                   fmt->scanline_pad,
                                   fmt->depth, fmt->bits_per_pixel, 0,
                                   QSysInfo::ByteOrder == QSysInfo::BigEndian ? XCB_IMAGE_ORDER_MSB_FIRST : XCB_IMAGE_ORDER_LSB_FIRST,
                                   XCB_IMAGE_ORDER_MSB_FIRST,
                                   0, ~0, 0);

    const int segmentSize = m_xcb_image->stride * m_xcb_image->height;
    if (!segmentSize)
        return;

    int id = -1;

    if (hasShm() && m_segmentSize > 0 && (m_segmentSize < segmentSize || m_segmentSize >= segmentSize * 4)) {
        doDestroyShm(true);
        m_shm_info.shmaddr = 0;
    }

    if (!hasShm()) {
        m_segmentSize = segmentSize;

        id = shmget(IPC_PRIVATE, m_segmentSize, IPC_CREAT | 0600);

        if (id == -1) {
            qWarning("QXcbShmImage: shmget() failed (%d: %s) for size %d (%dx%d)",
                     errno, strerror(errno), m_segmentSize, size.width(), size.height());
        } else {
            m_shm_info.shmaddr = m_xcb_image->data = (quint8 *)shmat(id, 0, 0);
        }

        m_shm_info.shmseg = xcb_generate_id(xcb_connection());
        m_shm_info.shmid = id;

        const xcb_query_extension_reply_t *shm_reply = xcb_get_extension_data(xcb_connection(), &xcb_shm_id);
        xcb_generic_error_t *error = NULL;
        bool shm_present = shm_reply != NULL && shm_reply->present;
        if (shm_present)
            error = xcb_request_check(xcb_connection(), xcb_shm_attach_checked(xcb_connection(), m_shm_info.shmseg, m_shm_info.shmid, false));
        if (!shm_present || error || id == -1) {
            free(error);

            if (id != -1)
                doDestroyShm(false);
            m_shm_info.shmaddr = 0;

            m_xcb_image->data = (uint8_t *)malloc(segmentSize);
        } else {
            if (shmctl(m_shm_info.shmid, IPC_RMID, 0) == -1)
                qWarning("QXcbBackingStore: Error while marking the shared memory segment to be destroyed");
        }
    } else {
        id = m_shm_info.shmid;
        m_xcb_image->data = m_shm_info.shmaddr;
    }

    m_qimage = QImage( (uchar*) m_xcb_image->data, m_xcb_image->width, m_xcb_image->height, m_xcb_image->stride, format);
    m_graphics_buffer = new QXcbShmGraphicsBuffer(&m_qimage);

    m_xcb_pixmap = xcb_generate_id(xcb_connection());
    xcb_create_pixmap(xcb_connection(),
                      m_xcb_image->depth,
                      m_xcb_pixmap,
                      m_screen_root,
                      m_xcb_image->width, m_xcb_image->height);
}

void QXcbShmImage::flushScrolledRegion(bool clientSideScroll)
{
    if (m_clientSideScroll == clientSideScroll)
       return;

    m_clientSideScroll = clientSideScroll;

    if (m_scrolledRegion.isNull())
        return;

    if (hasShm())
        connection()->sync();

    if (m_clientSideScroll) {
        // Copy scrolled image region from server-side pixmap to client-side memory
        for (const QRect &rect : m_scrolledRegion) {
            const int w = rect.width();
            const int h = rect.height();

            auto reply = Q_XCB_REPLY_UNCHECKED(xcb_get_image,
                                               xcb_connection(),
                                               m_xcb_image->format,
                                               m_xcb_pixmap,
                                               rect.x(), rect.y(),
                                               w, h,
                                               ~0u);

            if (reply && reply->depth == m_xcb_image->depth) {
                const QImage img(xcb_get_image_data(reply.get()), w, h, m_qimage.format());

                QPainter p(&m_qimage);
                p.setCompositionMode(QPainter::CompositionMode_Source);
                p.drawImage(rect.topLeft(), img);
            }
        }
        m_scrolledRegion = QRegion();
    } else {
        // Copy scrolled image region from client-side memory to server-side pixmap
        ensureGC(m_xcb_pixmap);
        if (hasShm())
            shmPutImage(m_xcb_pixmap, m_scrolledRegion);
        else
            flushPixmap(m_scrolledRegion, true);
    }
}

extern void qt_scrollRectInImage(QImage &img, const QRect &rect, const QPoint &offset);

bool QXcbShmImage::scroll(const QRegion &area, int dx, int dy)
{
    const QRect bounds(QPoint(), size());
    const QPoint delta(dx, dy);

    if (m_clientSideScroll) {
        if (m_qimage.isNull())
            return false;

        if (hasShm())
            preparePaint(area);

        for (const QRect &rect : area)
            qt_scrollRectInImage(m_qimage, rect, delta);
    } else {
        if (hasShm())
            shmPutImage(m_xcb_pixmap, m_pendingFlush.intersected(area));
        else
            flushPixmap(area);

        ensureGC(m_xcb_pixmap);

        for (const QRect &src : area) {
            const QRect dst = src.translated(delta).intersected(bounds);
            xcb_copy_area(xcb_connection(),
                          m_xcb_pixmap,
                          m_xcb_pixmap,
                          m_gc,
                          src.x(), src.y(),
                          dst.x(), dst.y(),
                          dst.width(), dst.height());
        }
    }

    m_scrolledRegion |= area.translated(delta).intersected(bounds);
    if (hasShm()) {
        m_pendingFlush -= area;
        m_pendingFlush -= m_scrolledRegion;
    }

    return true;
}

void QXcbShmImage::destroy(bool destroyShm)
{
    const int segmentSize = m_xcb_image ? (m_xcb_image->stride * m_xcb_image->height) : 0;

    if (segmentSize) {
        if (m_shm_info.shmaddr) {
            if (destroyShm)
                doDestroyShm(true);
        } else {
            free(m_xcb_image->data);
        }
    }

    xcb_image_destroy(m_xcb_image);

    if (m_gc) {
        xcb_free_gc(xcb_connection(), m_gc);
        m_gc = 0;
    }
    m_gc_drawable = 0;

    delete m_graphics_buffer;
    m_graphics_buffer = nullptr;

    xcb_free_pixmap(xcb_connection(), m_xcb_pixmap);
    m_xcb_pixmap = 0;
}

void QXcbShmImage::doDestroyShm(bool detachFromXcb)
{
    if (detachFromXcb)
        xcb_shm_detach(xcb_connection(), m_shm_info.shmseg);
    shmdt(m_shm_info.shmaddr);
    shmctl(m_shm_info.shmid, IPC_RMID, 0);
}

void QXcbShmImage::ensureGC(xcb_drawable_t dst)
{
    if (m_gc_drawable != dst) {
        if (m_gc)
            xcb_free_gc(xcb_connection(), m_gc);

        static const uint32_t mask = XCB_GC_GRAPHICS_EXPOSURES;
        static const uint32_t values[] = { 0 };

        m_gc = xcb_generate_id(xcb_connection());
        xcb_create_gc(xcb_connection(), m_gc, dst, mask, values);

        m_gc_drawable = dst;
    }
}

static inline void copy_unswapped(char *dst, int dstBytesPerLine, const QImage &img, const QRect &rect)
{
    const uchar *srcData = img.constBits();
    const int srcBytesPerLine = img.bytesPerLine();

    const int leftOffset = rect.left() * img.depth() >> 3;
    const int bottom = rect.bottom() + 1;

    for (int yy = rect.top(); yy < bottom; ++yy) {
        const uchar *src = srcData + yy * srcBytesPerLine + leftOffset;
        ::memmove(dst, src, dstBytesPerLine);
        dst += dstBytesPerLine;
    }
}

template <class Pixel>
static inline void copy_swapped(char *dst, const int dstStride, const QImage &img, const QRect &rect)
{
    const uchar *srcData = img.constBits();
    const int srcBytesPerLine = img.bytesPerLine();

    const int left = rect.left();
    const int width = rect.width();
    const int bottom = rect.bottom() + 1;

    for (int yy = rect.top(); yy < bottom; ++yy) {
        Pixel *dstPixels = reinterpret_cast<Pixel *>(dst);
        const Pixel *srcPixels = reinterpret_cast<const Pixel *>(srcData + yy * srcBytesPerLine) + left;

        for (int i = 0; i < width; ++i)
            dstPixels[i] = qbswap<Pixel>(*srcPixels++);

        dst += dstStride;
    }
}

static QImage native_sub_image(QByteArray *buffer, const int dstStride, const QImage &src, const QRect &rect, bool swap)
{
    if (!swap && src.rect() == rect && src.bytesPerLine() == dstStride)
        return src;

    buffer->resize(rect.height() * dstStride);

    if (swap) {
        switch (src.depth()) {
        case 32:
            copy_swapped<quint32>(buffer->data(), dstStride, src, rect);
            break;
        case 16:
            copy_swapped<quint16>(buffer->data(), dstStride, src, rect);
            break;
        }
    } else {
        copy_unswapped(buffer->data(), dstStride, src, rect);
    }

    return QImage(reinterpret_cast<const uchar *>(buffer->constData()), rect.width(), rect.height(), dstStride, src.format());
}

static inline quint32 round_up_scanline(quint32 base, quint32 pad)
{
    return (base + pad - 1) & -pad;
}

void QXcbShmImage::shmPutImage(xcb_drawable_t drawable, const QRegion &region, const QPoint offset)
{
    for (const QRect &rect : region) {
        const QPoint source = rect.translated(offset).topLeft();
        xcb_shm_put_image(xcb_connection(),
                          drawable,
                          m_gc,
                          m_xcb_image->width,
                          m_xcb_image->height,
                          source.x(), source.y(),
                          rect.width(), rect.height(),
                          rect.x(), rect.y(),
                          m_xcb_image->depth,
                          m_xcb_image->format,
                          0, // send event?
                          m_shm_info.shmseg,
                          m_xcb_image->data - m_shm_info.shmaddr);
    }
    m_dirtyShm |= region.translated(offset);
}

void QXcbShmImage::flushPixmap(const QRegion &region, bool fullRegion)
{
    QVector<QRect> rects;
    if (!fullRegion) {
        rects = m_pendingFlush.intersected(region).rects();
        m_pendingFlush -= region;
    } else {
        rects = region.rects();
    }

    xcb_image_t xcb_subimage;
    memset(&xcb_subimage, 0, sizeof(xcb_image_t));

    xcb_subimage.format = m_xcb_image->format;
    xcb_subimage.scanline_pad = m_xcb_image->scanline_pad;
    xcb_subimage.depth = m_xcb_image->depth;
    xcb_subimage.bpp = m_xcb_image->bpp;
    xcb_subimage.unit = m_xcb_image->unit;
    xcb_subimage.plane_mask = m_xcb_image->plane_mask;
    xcb_subimage.byte_order = (xcb_image_order_t) connection()->setup()->image_byte_order;
    xcb_subimage.bit_order = m_xcb_image->bit_order;

    const bool needsByteSwap = xcb_subimage.byte_order != m_xcb_image->byte_order;

    for (const QRect &rect : qAsConst(rects)) {
        // We must make sure that each request is not larger than max_req_size.
        // Each request takes req_size + m_xcb_image->stride * height bytes.
        static const uint32_t req_size = sizeof(xcb_put_image_request_t);
        const uint32_t max_req_size = xcb_get_maximum_request_length(xcb_connection());
        const int rows_per_put = (max_req_size - req_size) / m_xcb_image->stride;

        // This assert could trigger if a single row has more pixels than fit in
        // a single PutImage request. However, max_req_size is guaranteed to be
        // at least 16384 bytes. That should be enough for quite large images.
        Q_ASSERT(rows_per_put > 0);

        // If we upload the whole image in a single chunk, the result might be
        // larger than the server's maximum request size and stuff breaks.
        // To work around that, we upload the image in chunks where each chunk
        // is small enough for a single request.
        const int x = rect.x();
        int y = rect.y();
        const int width = rect.width();
        int height = rect.height();

        while (height > 0) {
            const int rows = std::min(height, rows_per_put);
            const QRect subRect(x, y, width, rows);
            const quint32 stride = round_up_scanline(width * m_qimage.depth(), xcb_subimage.scanline_pad) >> 3;
            const QImage subImage = native_sub_image(&m_flushBuffer, stride, m_qimage, subRect, needsByteSwap);

            xcb_subimage.width = width;
            xcb_subimage.height = rows;
            xcb_subimage.data = const_cast<uint8_t *>(subImage.constBits());
            xcb_image_annotate(&xcb_subimage);

            xcb_image_put(xcb_connection(),
                          m_xcb_pixmap,
                          m_gc,
                          &xcb_subimage,
                          x,
                          y,
                          0);

            y += rows;
            height -= rows;
        }
    }
}

void QXcbShmImage::setClip(const QRegion &region)
{
    if (region.isEmpty()) {
        static const uint32_t mask = XCB_GC_CLIP_MASK;
        static const uint32_t values[] = { XCB_NONE };
        xcb_change_gc(xcb_connection(), m_gc, mask, values);
    } else {
        const QVector<QRect> qrects = region.rects();
        QVector<xcb_rectangle_t> xcb_rects(qrects.size());

        for (int i = 0; i < qrects.size(); i++) {
            xcb_rects[i].x = qrects[i].x();
            xcb_rects[i].y = qrects[i].y();
            xcb_rects[i].width = qrects[i].width();
            xcb_rects[i].height = qrects[i].height();
        }

        xcb_set_clip_rectangles(xcb_connection(),
                                XCB_CLIP_ORDERING_YX_BANDED,
                                m_gc,
                                0, 0,
                                xcb_rects.size(), xcb_rects.constData());
    }
}

void QXcbShmImage::put(xcb_drawable_t dst, const QRegion &region, const QPoint &offset)
{
    Q_ASSERT(!m_clientSideScroll);

    ensureGC(dst);
    setClip(region);

    if (hasShm()) {
        // Copy scrolled area on server-side from pixmap to window
        const QRegion scrolledRegion = m_scrolledRegion.translated(-offset);
        for (const QRect &rect : scrolledRegion) {
            const QPoint source = rect.translated(offset).topLeft();
            xcb_copy_area(xcb_connection(),
                          m_xcb_pixmap,
                          dst,
                          m_gc,
                          source.x(), source.y(),
                          rect.x(), rect.y(),
                          rect.width(), rect.height());
        }

        // Copy non-scrolled image from client-side memory to server-side window
        const QRegion notScrolledArea = region - scrolledRegion;
        shmPutImage(dst, notScrolledArea, offset);
    } else {
        const QRect bounds = region.boundingRect();
        const QPoint target = bounds.topLeft();
        const QRect source = bounds.translated(offset);
        flushPixmap(region);
        xcb_copy_area(xcb_connection(),
                      m_xcb_pixmap,
                      dst,
                      m_gc,
                      source.x(), source.y(),
                      target.x(), target.y(),
                      source.width(), source.height());
    }

    setClip(QRegion());
}

void QXcbShmImage::preparePaint(const QRegion &region)
{
    if (hasShm()) {
        // to prevent X from reading from the image region while we're writing to it
        if (m_dirtyShm.intersects(region)) {
            connection()->sync();
            m_dirtyShm = QRegion();
        }
    }
    m_scrolledRegion -= region;
    m_pendingFlush |= region;
}

QXcbBackingStore::QXcbBackingStore(QWindow *window)
    : QPlatformBackingStore(window)
    , m_image(0)
{
    QXcbScreen *screen = static_cast<QXcbScreen *>(window->screen()->handle());
    setConnection(screen->connection());
}

QXcbBackingStore::~QXcbBackingStore()
{
    delete m_image;
}

QPaintDevice *QXcbBackingStore::paintDevice()
{
    if (!m_image)
        return 0;
    return m_rgbImage.isNull() ? m_image->image() : &m_rgbImage;
}

void QXcbBackingStore::beginPaint(const QRegion &region)
{
    if (!m_image)
        return;

    m_paintRegions.push(region);
    m_image->preparePaint(region);

    if (m_image->hasAlpha()) {
        QPainter p(paintDevice());
        p.setCompositionMode(QPainter::CompositionMode_Source);
        const QColor blank = Qt::transparent;
        for (const QRect &rect : region)
            p.fillRect(rect, blank);
    }
}

void QXcbBackingStore::endPaint()
{
    if (Q_UNLIKELY(m_paintRegions.isEmpty())) {
        qWarning("%s: paint regions empty!", Q_FUNC_INFO);
        return;
    }

    const QRegion region = m_paintRegions.pop();
    m_image->preparePaint(region);

    QXcbWindow *platformWindow = static_cast<QXcbWindow *>(window()->handle());
    if (!platformWindow || !platformWindow->imageNeedsRgbSwap())
        return;

    // Slow path: the paint device was m_rgbImage. Now copy with swapping red
    // and blue into m_image.
    auto it = region.begin();
    const auto end = region.end();
    if (it == end)
        return;
    QPainter p(m_image->image());
    while (it != end) {
        const QRect rect = *(it++);
        p.drawImage(rect.topLeft(), m_rgbImage.copy(rect).rgbSwapped());
    }
}

QImage QXcbBackingStore::toImage() const
{
    return m_image && m_image->image() ? *m_image->image() : QImage();
}

QPlatformGraphicsBuffer *QXcbBackingStore::graphicsBuffer() const
{
    return m_image ? m_image->graphicsBuffer() : nullptr;
}

void QXcbBackingStore::flush(QWindow *window, const QRegion &region, const QPoint &offset)
{
    if (!m_image || m_image->size().isEmpty())
        return;

    m_image->flushScrolledRegion(false);

    QSize imageSize = m_image->size();

    QRegion clipped = region;
    clipped &= QRect(QPoint(), QHighDpi::toNativePixels(window->size(), window));
    clipped &= QRect(0, 0, imageSize.width(), imageSize.height()).translated(-offset);

    QRect bounds = clipped.boundingRect();

    if (bounds.isNull())
        return;

    QXcbWindow *platformWindow = static_cast<QXcbWindow *>(window->handle());
    if (!platformWindow) {
        qWarning("QXcbBackingStore::flush: QWindow has no platform window (QTBUG-32681)");
        return;
    }

    m_image->put(platformWindow->xcb_window(), clipped, offset);

    if (platformWindow->needsSync())
        platformWindow->updateSyncRequestCounter();
    else
        xcb_flush(xcb_connection());
}

#ifndef QT_NO_OPENGL
void QXcbBackingStore::composeAndFlush(QWindow *window, const QRegion &region, const QPoint &offset,
                                       QPlatformTextureList *textures,
                                       bool translucentBackground)
{
    if (!m_image || m_image->size().isEmpty())
        return;

    m_image->flushScrolledRegion(true);

    QPlatformBackingStore::composeAndFlush(window, region, offset, textures, translucentBackground);

    QXcbWindow *platformWindow = static_cast<QXcbWindow *>(window->handle());
    if (platformWindow->needsSync()) {
        platformWindow->updateSyncRequestCounter();
    } else {
        xcb_flush(xcb_connection());
    }
}
#endif // QT_NO_OPENGL

void QXcbBackingStore::resize(const QSize &size, const QRegion &)
{
    if (m_image && size == m_image->size())
        return;

    QPlatformWindow *pw = window()->handle();
    if (!pw) {
        window()->create();
        pw = window()->handle();
    }
    QXcbWindow* win = static_cast<QXcbWindow *>(pw);

    if (m_image) {
        m_image->resize(size);
    } else {
        QXcbScreen *screen = static_cast<QXcbScreen *>(window()->screen()->handle());
        m_image = new QXcbShmImage(screen, size, win->depth(), win->imageFormat());
    }

    // Slow path for bgr888 VNC: Create an additional image, paint into that and
    // swap R and B while copying to m_image after each paint.
    if (win->imageNeedsRgbSwap()) {
        m_rgbImage = QImage(size, win->imageFormat());
    }
}

bool QXcbBackingStore::scroll(const QRegion &area, int dx, int dy)
{
    if (m_image)
        return m_image->scroll(area, dx, dy);

    return false;
}

QT_END_NAMESPACE
