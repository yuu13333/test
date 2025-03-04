/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
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

#include "androidjniaccessibility.h"
#include "androidjnimain.h"
#include "qandroidplatformintegration.h"
#include "qpa/qplatformaccessibility.h"
#include <QtGui/private/qaccessiblebridgeutils_p.h>
#include "qguiapplication.h"
#include "qwindow.h"
#include "qrect.h"
#include "QtGui/qaccessible.h"
#include <QtCore/qmath.h>
#include <QtCore/private/qjnihelpers_p.h>
#include <QtCore/QJniObject>
#include <QtGui/private/qhighdpiscaling_p.h>
#include <QtCore/QObject>

static const char m_qtTag[] = "Qt A11Y";
static const char m_classErrorMsg[] = "Can't find class \"%s\"";

QT_BEGIN_NAMESPACE

namespace QtAndroidAccessibility
{
    static jmethodID m_addActionMethodID = 0;
    static jmethodID m_setCheckableMethodID = 0;
    static jmethodID m_setCheckedMethodID = 0;
    static jmethodID m_setClickableMethodID = 0;
    static jmethodID m_setContentDescriptionMethodID = 0;
    static jmethodID m_setEditableMethodID = 0;
    static jmethodID m_setEnabledMethodID = 0;
    static jmethodID m_setFocusableMethodID = 0;
    static jmethodID m_setFocusedMethodID = 0;
    static jmethodID m_setScrollableMethodID = 0;
    static jmethodID m_setTextSelectionMethodID = 0;
    static jmethodID m_setVisibleToUserMethodID = 0;

    static bool m_accessibilityActivated = false;

    // This object is needed to schedule the execution of the code that
    // deals with accessibility instances to the Qt main thread.
    // Because of that almost every method here is split into two parts.
    // The _helper part is executed in the context of m_accessibilityContext
    // on the main thread. The other part is executed in Java thread.
    static QObject *m_accessibilityContext = nullptr;

    // This method is called from the Qt main thread, and normally a
    // QGuiApplication instance will be used as a parent.
    void createAccessibilityContextObject(QObject *parent)
    {
        if (m_accessibilityContext)
            m_accessibilityContext->deleteLater();
        m_accessibilityContext = new QObject(parent);
    }

    template <typename Func, typename Ret>
    void runInObjectContext(QObject *context, Func &&func, Ret *retVal)
    {
        QMetaObject::invokeMethod(context, func, Qt::BlockingQueuedConnection, retVal);
    }

    void initialize()
    {
        QJniObject::callStaticMethod<void>(QtAndroid::applicationClass(),
                                           "initializeAccessibility");
    }

    bool isActive()
    {
        return m_accessibilityActivated;
    }

    static void setActive(JNIEnv */*env*/, jobject /*thiz*/, jboolean active)
    {
        QMutexLocker lock(QtAndroid::platformInterfaceMutex());
        QAndroidPlatformIntegration *platformIntegration = QtAndroid::androidPlatformIntegration();
        m_accessibilityActivated = active;
        if (platformIntegration)
            platformIntegration->accessibility()->setActive(active);
        else
            __android_log_print(ANDROID_LOG_WARN, m_qtTag, "Could not (yet) activate platform accessibility.");
    }

    QAccessibleInterface *interfaceFromId(jint objectId)
    {
        QAccessibleInterface *iface = nullptr;
        if (objectId == -1) {
            QWindow *win = qApp->focusWindow();
            if (win)
                iface = win->accessibleRoot();
        } else {
            iface = QAccessible::accessibleInterface(objectId);
        }
        return iface;
    }

    void notifyLocationChange()
    {
        QtAndroid::notifyAccessibilityLocationChange();
    }

    static int parentId_helper(int objectId); // forward declaration

    void notifyObjectHide(uint accessibilityObjectId)
    {
        const auto parentObjectId = parentId_helper(accessibilityObjectId);
        QtAndroid::notifyObjectHide(accessibilityObjectId, parentObjectId);
    }

    void notifyObjectFocus(uint accessibilityObjectId)
    {
        QtAndroid::notifyObjectFocus(accessibilityObjectId);
    }

    static QVarLengthArray<int, 8> childIdListForAccessibleObject_helper(int objectId)
    {
        QAccessibleInterface *iface = interfaceFromId(objectId);
        if (iface && iface->isValid()) {
            const int childCount = iface->childCount();
            QVarLengthArray<jint, 8> ifaceIdArray;
            ifaceIdArray.reserve(childCount);
            for (int i = 0; i < childCount; ++i) {
                QAccessibleInterface *child = iface->child(i);
                if (child && child->isValid())
                    ifaceIdArray.append(QAccessible::uniqueId(child));
            }
            return ifaceIdArray;
        }
        return {};
    }

    static jintArray childIdListForAccessibleObject(JNIEnv *env, jobject /*thiz*/, jint objectId)
    {
        if (m_accessibilityContext) {
            QVarLengthArray<jint, 8> ifaceIdArray;
            runInObjectContext(m_accessibilityContext, [objectId]() {
                return childIdListForAccessibleObject_helper(objectId);
            }, &ifaceIdArray);
            jintArray jArray = env->NewIntArray(jsize(ifaceIdArray.count()));
            env->SetIntArrayRegion(jArray, 0, ifaceIdArray.count(), ifaceIdArray.data());
            return jArray;
        }

        return env->NewIntArray(jsize(0));
    }

    static int parentId_helper(int objectId)
    {
        QAccessibleInterface *iface = interfaceFromId(objectId);
        if (iface && iface->isValid()) {
            QAccessibleInterface *parent = iface->parent();
            if (parent && parent->isValid()) {
                if (parent->role() == QAccessible::Application)
                    return -1;
                return QAccessible::uniqueId(parent);
            }
        }
        return -1;
    }

    static jint parentId(JNIEnv */*env*/, jobject /*thiz*/, jint objectId)
    {
        jint result = -1;
        if (m_accessibilityContext) {
            runInObjectContext(m_accessibilityContext, [objectId]() {
                return parentId_helper(objectId);
            }, &result);
        }
        return result;
    }

    static QRect screenRect_helper(int objectId)
    {
        QRect rect;
        QAccessibleInterface *iface = interfaceFromId(objectId);
        if (iface && iface->isValid()) {
            rect = QHighDpi::toNativePixels(iface->rect(), iface->window());
        }
        // If the widget is not fully in-bound in its parent then we have to clip the rectangle to draw
        if (iface && iface->parent() && iface->parent()->isValid()) {
            const auto parentRect = QHighDpi::toNativePixels(iface->parent()->rect(), iface->parent()->window());
            rect = rect.intersected(parentRect);
        }
        return rect;
    }

    static jobject screenRect(JNIEnv *env, jobject /*thiz*/, jint objectId)
    {
        QRect rect;
        if (m_accessibilityContext) {
            runInObjectContext(m_accessibilityContext, [objectId]() {
                return screenRect_helper(objectId);
            }, &rect);
        }
        jclass rectClass = env->FindClass("android/graphics/Rect");
        jmethodID ctor = env->GetMethodID(rectClass, "<init>", "(IIII)V");
        jobject jrect = env->NewObject(rectClass, ctor, rect.left(), rect.top(), rect.right(), rect.bottom());
        return jrect;
    }

    static int hitTest_helper(float x, float y)
    {
        QAccessibleInterface *root = interfaceFromId(-1);
        if (root && root->isValid()) {
            QPoint pos = QHighDpi::fromNativePixels(QPoint(int(x), int(y)), root->window());

            QAccessibleInterface *child = root->childAt(pos.x(), pos.y());
            QAccessibleInterface *lastChild = nullptr;
            while (child && (child != lastChild)) {
                lastChild = child;
                child = child->childAt(pos.x(), pos.y());
            }
            if (lastChild)
                return QAccessible::uniqueId(lastChild);
        }
        return -1;
    }

    static jint hitTest(JNIEnv */*env*/, jobject /*thiz*/, jfloat x, jfloat y)
    {
        jint result = -1;
        if (m_accessibilityContext) {
            runInObjectContext(m_accessibilityContext, [x, y]() {
                return hitTest_helper(x, y);
            }, &result);
        }
        return result;
    }

    static void invokeActionOnInterfaceInMainThread(QAccessibleActionInterface* actionInterface,
                                                    const QString& action)
    {
        // Queue the action and return back to Java thread, so that we do not
        // block it for too long
        QMetaObject::invokeMethod(qApp, [actionInterface, action]() {
            actionInterface->doAction(action);
        }, Qt::QueuedConnection);
    }

    static bool clickAction_helper(int objectId)
    {
        QAccessibleInterface *iface = interfaceFromId(objectId);
        if (!iface || !iface->isValid() || !iface->actionInterface())
            return false;

        const auto& actionNames = iface->actionInterface()->actionNames();

        if (actionNames.contains(QAccessibleActionInterface::pressAction())) {
            invokeActionOnInterfaceInMainThread(iface->actionInterface(),
                                                QAccessibleActionInterface::pressAction());
        } else if (actionNames.contains(QAccessibleActionInterface::toggleAction())) {
            invokeActionOnInterfaceInMainThread(iface->actionInterface(),
                                                QAccessibleActionInterface::toggleAction());
        } else {
            return false;
        }
        return true;
    }

    static jboolean clickAction(JNIEnv */*env*/, jobject /*thiz*/, jint objectId)
    {
        bool result = false;
        if (m_accessibilityContext) {
            runInObjectContext(m_accessibilityContext, [objectId]() {
                return clickAction_helper(objectId);
            }, &result);
        }
        return result;
    }

    static bool scroll_helper(int objectId, const QString &actionName)
    {
        QAccessibleInterface *iface = interfaceFromId(objectId);
        if (iface && iface->isValid())
            return QAccessibleBridgeUtils::performEffectiveAction(iface, actionName);
        return false;
    }

    static jboolean scrollForward(JNIEnv */*env*/, jobject /*thiz*/, jint objectId)
    {
        bool result = false;
        if (m_accessibilityContext) {
            runInObjectContext(m_accessibilityContext, [objectId]() {
                return scroll_helper(objectId, QAccessibleActionInterface::increaseAction());
            }, &result);
        }
        return result;
    }

    static jboolean scrollBackward(JNIEnv */*env*/, jobject /*thiz*/, jint objectId)
    {
        bool result = false;
        if (m_accessibilityContext) {
            runInObjectContext(m_accessibilityContext, [objectId]() {
                return scroll_helper(objectId, QAccessibleActionInterface::decreaseAction());
            }, &result);
        }
        return result;
    }


#define FIND_AND_CHECK_CLASS(CLASS_NAME) \
clazz = env->FindClass(CLASS_NAME); \
if (!clazz) { \
    __android_log_print(ANDROID_LOG_FATAL, m_qtTag, m_classErrorMsg, CLASS_NAME); \
    return JNI_FALSE; \
}

        //__android_log_print(ANDROID_LOG_FATAL, m_qtTag, m_methodErrorMsg, METHOD_NAME, METHOD_SIGNATURE);


    static QString descriptionForInterface(QAccessibleInterface *iface)
    {
        QString desc;
        if (iface && iface->isValid()) {
            desc = iface->text(QAccessible::Name);
            if (desc.isEmpty())
                desc = iface->text(QAccessible::Description);
            if (desc.isEmpty()) {
                desc = iface->text(QAccessible::Value);
                if (desc.isEmpty()) {
                    if (QAccessibleValueInterface *valueIface = iface->valueInterface()) {
                        desc= valueIface->currentValue().toString();
                    }
                }
            }
        }
        return desc;
    }

    static QString descriptionForAccessibleObject_helper(int objectId)
    {
        QAccessibleInterface *iface = interfaceFromId(objectId);
        return descriptionForInterface(iface);
    }

    static jstring descriptionForAccessibleObject(JNIEnv *env, jobject /*thiz*/, jint objectId)
    {
        QString desc;
        if (m_accessibilityContext) {
            runInObjectContext(m_accessibilityContext, [objectId]() {
                return descriptionForAccessibleObject_helper(objectId);
            }, &desc);
        }
        return env->NewString((jchar*) desc.constData(), (jsize) desc.size());
    }


    struct NodeInfo
    {
        bool valid = false;
        QAccessible::State state;
        QStringList actions;
        QString description;
        bool hasTextSelection = false;
        int selectionStart = 0;
        int selectionEnd = 0;
    };

    static NodeInfo populateNode_helper(int objectId)
    {
        NodeInfo info;
        QAccessibleInterface *iface = interfaceFromId(objectId);
        if (iface && iface->isValid()) {
            info.valid = true;
            info.state = iface->state();
            info.actions = QAccessibleBridgeUtils::effectiveActionNames(iface);
            info.description = descriptionForInterface(iface);
            QAccessibleTextInterface *textIface = iface->textInterface();
            if (textIface && (textIface->selectionCount() > 0)) {
                info.hasTextSelection = true;
                textIface->selection(0, &info.selectionStart, &info.selectionEnd);
            }
        }
        return info;
    }

    static jboolean populateNode(JNIEnv *env, jobject /*thiz*/, jint objectId, jobject node)
    {
        NodeInfo info;
        if (m_accessibilityContext) {
            runInObjectContext(m_accessibilityContext, [objectId]() {
                return populateNode_helper(objectId);
            }, &info);
        }
        if (!info.valid) {
            __android_log_print(ANDROID_LOG_WARN, m_qtTag, "Accessibility: populateNode for Invalid ID");
            return false;
        }

        const bool hasClickableAction =
                info.actions.contains(QAccessibleActionInterface::pressAction()) ||
                info.actions.contains(QAccessibleActionInterface::toggleAction());
        const bool hasIncreaseAction =
                info.actions.contains(QAccessibleActionInterface::increaseAction());
        const bool hasDecreaseAction =
                info.actions.contains(QAccessibleActionInterface::decreaseAction());

        if (info.hasTextSelection && m_setTextSelectionMethodID) {
            env->CallVoidMethod(node, m_setTextSelectionMethodID, info.selectionStart,
                                info.selectionEnd);
        }

        env->CallVoidMethod(node, m_setCheckableMethodID, (bool)info.state.checkable);
        env->CallVoidMethod(node, m_setCheckedMethodID, (bool)info.state.checked);
        env->CallVoidMethod(node, m_setEditableMethodID, info.state.editable);
        env->CallVoidMethod(node, m_setEnabledMethodID, !info.state.disabled);
        env->CallVoidMethod(node, m_setFocusableMethodID, (bool)info.state.focusable);
        env->CallVoidMethod(node, m_setFocusedMethodID, (bool)info.state.focused);
        env->CallVoidMethod(node, m_setVisibleToUserMethodID, !info.state.invisible);
        env->CallVoidMethod(node, m_setScrollableMethodID, hasIncreaseAction || hasDecreaseAction);
        env->CallVoidMethod(node, m_setClickableMethodID, hasClickableAction);

        // Add ACTION_CLICK
        if (hasClickableAction)
            env->CallVoidMethod(node, m_addActionMethodID, (int)0x00000010);    // ACTION_CLICK defined in AccessibilityNodeInfo

        // Add ACTION_SCROLL_FORWARD
        if (hasIncreaseAction)
            env->CallVoidMethod(node, m_addActionMethodID, (int)0x00001000);    // ACTION_SCROLL_FORWARD defined in AccessibilityNodeInfo

        // Add ACTION_SCROLL_BACKWARD
        if (hasDecreaseAction)
            env->CallVoidMethod(node, m_addActionMethodID, (int)0x00002000);    // ACTION_SCROLL_BACKWARD defined in AccessibilityNodeInfo

        // try to fill in the text property, this is what the screen reader reads
        jstring jdesc = env->NewString((jchar*)info.description.constData(),
                                       (jsize)info.description.size());
        //CALL_METHOD(node, "setText", "(Ljava/lang/CharSequence;)V", jdesc)
        env->CallVoidMethod(node, m_setContentDescriptionMethodID, jdesc);

        return true;
    }

    static JNINativeMethod methods[] = {
        {"setActive","(Z)V",(void*)setActive},
        {"childIdListForAccessibleObject", "(I)[I", (jintArray)childIdListForAccessibleObject},
        {"parentId", "(I)I", (void*)parentId},
        {"descriptionForAccessibleObject", "(I)Ljava/lang/String;", (jstring)descriptionForAccessibleObject},
        {"screenRect", "(I)Landroid/graphics/Rect;", (jobject)screenRect},
        {"hitTest", "(FF)I", (void*)hitTest},
        {"populateNode", "(ILandroid/view/accessibility/AccessibilityNodeInfo;)Z", (void*)populateNode},
        {"clickAction", "(I)Z", (void*)clickAction},
        {"scrollForward", "(I)Z", (void*)scrollForward},
        {"scrollBackward", "(I)Z", (void*)scrollBackward},
    };

#define GET_AND_CHECK_STATIC_METHOD(VAR, CLASS, METHOD_NAME, METHOD_SIGNATURE) \
    VAR = env->GetMethodID(CLASS, METHOD_NAME, METHOD_SIGNATURE); \
    if (!VAR) { \
        __android_log_print(ANDROID_LOG_FATAL, QtAndroid::qtTagText(), QtAndroid::methodErrorMsgFmt(), METHOD_NAME, METHOD_SIGNATURE); \
        return false; \
    }

    bool registerNatives(JNIEnv *env)
    {
        jclass clazz;
        FIND_AND_CHECK_CLASS("org/qtproject/qt/android/accessibility/QtNativeAccessibility");
        jclass appClass = static_cast<jclass>(env->NewGlobalRef(clazz));

        if (env->RegisterNatives(appClass, methods, sizeof(methods) / sizeof(methods[0])) < 0) {
            __android_log_print(ANDROID_LOG_FATAL,"Qt A11y", "RegisterNatives failed");
            return false;
        }

        jclass nodeInfoClass = env->FindClass("android/view/accessibility/AccessibilityNodeInfo");
        GET_AND_CHECK_STATIC_METHOD(m_addActionMethodID, nodeInfoClass, "addAction", "(I)V");
        GET_AND_CHECK_STATIC_METHOD(m_setCheckableMethodID, nodeInfoClass, "setCheckable", "(Z)V");
        GET_AND_CHECK_STATIC_METHOD(m_setCheckedMethodID, nodeInfoClass, "setChecked", "(Z)V");
        GET_AND_CHECK_STATIC_METHOD(m_setClickableMethodID, nodeInfoClass, "setClickable", "(Z)V");
        GET_AND_CHECK_STATIC_METHOD(m_setContentDescriptionMethodID, nodeInfoClass, "setContentDescription", "(Ljava/lang/CharSequence;)V");
        GET_AND_CHECK_STATIC_METHOD(m_setEditableMethodID, nodeInfoClass, "setEditable", "(Z)V");
        GET_AND_CHECK_STATIC_METHOD(m_setEnabledMethodID, nodeInfoClass, "setEnabled", "(Z)V");
        GET_AND_CHECK_STATIC_METHOD(m_setFocusableMethodID, nodeInfoClass, "setFocusable", "(Z)V");
        GET_AND_CHECK_STATIC_METHOD(m_setFocusedMethodID, nodeInfoClass, "setFocused", "(Z)V");
        GET_AND_CHECK_STATIC_METHOD(m_setScrollableMethodID, nodeInfoClass, "setScrollable", "(Z)V");
        GET_AND_CHECK_STATIC_METHOD(m_setVisibleToUserMethodID, nodeInfoClass, "setVisibleToUser", "(Z)V");
        GET_AND_CHECK_STATIC_METHOD(m_setTextSelectionMethodID, nodeInfoClass, "setTextSelection", "(II)V");

        return true;
    }
}

QT_END_NAMESPACE
