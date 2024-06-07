/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtCore module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/
#include "qcalendar.h"
#include "qcalendarbackend_p.h"
#include "qgregoriancalendar_p.h"

#include "qdatetime.h"
#include "qcalendarmath_p.h"
#include <qdebug.h>

QT_BEGIN_NAMESPACE

QMap<QString, QCalendarBackend *> QCalendarBackend::s_calendars;

/*!
    \since 5.14

    \class QCalendarBackend
    \inmodule QtCore
    \reentrant
    \brief The QCalendarBackend class provides basic calendaring functions.

    QCalendarBackend provides the base class on which all calendar types are
    implemented. On construction, the backend is registered with its primary
    name. It may also be registered with various other names, where the calendar
    is known by several aliases. Registering with the name used by CLDR (the
    Unicode consortium's Common Locale Data Repository) is recommended,
    particularly when interacting with third-party software. The backend is
    deregistered upon destruction. While a backend is registered for a name,
    QCalendar can be constructed using that name to select the backend.

    Each calendar backend must inherit from QCalendarBackend and implement its
    pure virtual methods. It may also override some other virtual methods, as
    needed.

    Most backends are pure code, with no data elements. Such backends should
    normally be implemented as singletons. For a backend to be added to the
    QCalendar::System enum, it should be such a singleton, with an instance()
    method that returns the single instance (and no public constructor).

    Non-singleton calendar backends should ensure that each instance registers
    with a distinct primary name. Later instances attempting to register with a
    name already in use shall fail to register and be unavailable to QCalendar,
    hence unusable.

    \sa QDate, QDateTime, QDateEdit, QDateTimeEdit, QCalendarWidget
*/

/*!
    Constructs the calendar and registers it.
*/
QCalendarBackend::QCalendarBackend(const QString &name) : m_name(name)
{
    registerCalendar(name);
}

/*!
    Destroys the calendar.

    Deregisters it in the process.
*/
QCalendarBackend::~QCalendarBackend()
{
    auto dead =
        std::remove_if(s_calendars.begin(), s_calendars.end(),
                       [this] (QCalendarBackend *it) -> bool {
                           return it == this;
                       });
    while (dead != s_calendars.end())
        dead = s_calendars.erase(dead);
}

/*!
  The primary name of this calendar.
 */
const QString &QCalendarBackend::name() const { return m_name; }
QStringView QCalendar::name() const { return d ? d->name() : QString(); }

// date queries
/*!
   \fn int QCalendarBackend::daysInMonth(int month, int year) const

   Returns number of days in the month number \a month, in year \a year.

   An implementation should return 0 if the given year had no such month. If
   year is QCalendar::Unspecified, return the usual number of days for the
   month, in those years that include it.

   Calendars with intercallary days may represent these as extra days of the
   preceding month, or as short months separate from the usual ones. In the
   former case, daysInMonth(month, year) should be the number of ordinary days
   in the month, although \c{isValid(year, month, day)} might return \c true for
   some larger values of \c day.

   \sa daysInYear(), monthsInYear(), minDaysInMonth(), maxDaysInMonth()
*/

// properties of the calendar

/*!
    \fn bool QCalendarBackend::isLeapYear(int year) const

    Returns \c true if the specified \a year is a leap year for this calendar.

    \sa daysInYear(), isValid()
*/

/*!
    \fn bool QCalendarBackend::isLunar() const

    Returns \c true if this calendar is a lunar calendar. Otherwise returns \c
    false.

    A lunar calendar is a calendar based upon the monthly cycles of the Moon's
    phases (synodic months). This contrasts with solar calendars, whose annual
    cycles are based only upon the solar year.

    \sa isLuniSolar(), isSolar(), isProleptic()
*/

/*!
    \fn bool QCalendarBackend::isLuniSolar() const

    Returns \c true if this calendar is a lunisolar calendar. Otherwise returns
    \c false.

    A lunisolar calendar is a calendar whose date indicates both the moon phase
    and the time of the solar year.

    \sa isLunar(), isSolar(), isProleptic()
*/

/*!
    \fn bool QCalendarBackend::isSolar() const

    Returns \c true if this calendar is a solar calendar. Otherwise returns
    \c false.

    A solar calendar is a calendar whose dates indicate the season or almost
    equivalently the apparent position of the sun relative to the fixed stars.
    The Gregorian calendar, widely accepted as standard in the world,
    is an example of solar calendar.

    \sa isLuniSolar(), isLunar(), isProleptic()
*/

/*!
    Returns the total number of days in the year number \a year.
    Returns zero if there is no such year in this calendar.

    This base implementation returns 366 for leap years and 365 for ordinary
    years.

    \sa monthsInYear(), daysInMonth(), isLeapYear()
*/
int QCalendarBackend::daysInYear(int year) const
{
    return monthsInYear(year) ? isLeapYear(year) ? 366 : 365 : 0;
}

/*!
    Returns the total number of months in the year number \a year.
    Returns zero if there is no such year in this calendar.

    This base implementation returns 12 for any valid year.

    \sa daysInYear(), maxMonthsInYear(), isValid()
*/
int QCalendarBackend::monthsInYear(int year) const
{
    return year > 0 || (year < 0 ? isProleptic() : hasYearZero()) ? 12 : 0;
}

/*!
    Returns \c true if the date specified by \a year, \a month and \a day is
    valid for this calendar; otherwise returns \c false. For example,
    the date 2018-04-19 is valid for the Gregorian calendar, but 2018-16-19 and
    2018-04-38 are invalid.

    Calendars with intercallary days may represent these as extra days of the
    preceding month or as short months separate from the usual ones. In the
    former case, a \a day value greater than \c{daysInMonth(\a{month},
    \a{year})} may be valid.

    \sa daysInMonth(), monthsInYear()
*/
bool QCalendarBackend::isValid(int year, int month, int day) const
{
    return day > 0 && day <= daysInMonth(month, year);
}

/*!
    Returns \c true if this calendar is a proleptic calendar. Otherwise returns
    \c false.

    A proleptic calendar results from allowing negative year numbers to indicate
    years before the nominal start of the calendar system.

    \sa isLuniSolar(), isSolar(), isLunar(), hasYearZero()
*/

bool QCalendarBackend::isProleptic() const
{
    return true;
}

/*!
    Returns \c true if year number \c 0 is considered a valid year in this
    calendar. Otherwise returns \c false.

    \sa isValid(), isProleptic()
*/

bool QCalendarBackend::hasYearZero() const
{
    return false;
}

/*!
    Returns the maximum number of days in a month for any year.

    This base implementation returns 31, as this is a common case.

    For calendars with intercallary days, although daysInMonth() doesn't include
    the intercallary days in its count for an individual month, maxDaysInMonth()
    should include intercallary days, so that it is the maximum value of \c day
    for which \c{isValid(year, month, day)} can be true.

    \sa maxMonthsInYear(), daysInMonth()
*/
int QCalendarBackend::maxDaysInMonth() const
{
    return 31;
}

/*!
    Returns the minimum number of days in any valid month of any valid year.

    This base implementation returns 29, as this is a common case.

    \sa maxMonthsInYear(), daysInMonth()
*/
int QCalendarBackend::minDaysInMonth() const
{
    return 29;
}

/*!
    Returns the maximum number of months possible in any year.

    This base implementation returns 12, as this is a common case.

    \sa maxDaysInMonth(), monthsInYear()
*/
int QCalendarBackend::maxMonthsInYear() const
{
    return 12;
}

/*!
   Returns the day of the week for a given Julian Day Number.

   This is 1 for Monday through 7 for Sunday.

   Calendars with intercallary days may return larger values for these
   intercallary days. They should avoid using 0 for any special purpose (it is
   already used in QDate::dayOfWeek() to mean an invalid date). The calendar
   should treat the numbers used as an \c enum, whose values need not be
   contiguous, nor need they follow closely from the 1 through 7 of the usual
   returns. It suffices that weekDayName() can recognize each such number as
   identifying a distinct name, that it returns to identify the particular
   intercallary day.

   This base implementation uses the day-numbering that various calendars have
   borrowed off the Hebrew calendar.

   \sa weekDayName(), standaloneWeekDayName(), QDate::dayOfWeek()
 */
int QCalendarBackend::dayOfWeek(qint64 jd) const
{
    return QRoundingDown::qMod(jd, 7) + 1;
}

// Month and week-day name look-ups (implemented in qlocale.cpp):
/*!
  \fn QString QCalendarBackend::monthName(const QLocale &locale, int month, int year,
                                          QLocale::FormatType format) const

  Returns the name of the specified \a month in the given \a year for the chosen
  \a locale, using the given \a format to determine how complete the name is.

  If \a year is Unspecified, return the name for the month that usually has this
  number within a typical year. Calendars with a leap month that isn't always
  the last may need to take account of the year to map the month number to the
  particular year's month with that number.

  \note Backends for which CLDR provides data can configure the default
  implementation of the two month name look-up methods by arranging for
  localeMonthIndexData() and localeMonthData() to provide access to the CLDR
  data (see cldr2qlocalexml.py, qlocalexml2cpp.py and existing backends).
  Conversely, backends that override both month name look-up methods need not
  return anything meaningful from localeMonthIndexData() or localeMonthData().

  \sa standaloneMonthName(), QLocale::monthName()
*/

/*!
  \fn QString QCalendarBackend::standaloneMonthName(const QLocale &locale, int month, int year
                                                    QLocale::FormatType format) const

  Returns the standalone name of the specified \a month in the chosen \a locale,
  using the specified \a format to determine how complete the name is.

  If \a year is Unspecified, return the standalone name for the month that
  usually has this number within a typical year. Calendars with a leap month
  that isn't always the last may need to take account of the year to map the
  month number to the particular year's month with that number.

  \sa monthName(), QLocale::standaloneMonthName()
*/

/*!
  \fn QString QCalendarBackend::weekDayName(const QLocale &locale, int day,
                                            QLocale::FormatType format) const

  Returns the name of the specified \a day of the week in the chosen \a locale,
  using the specified \a format to determine how complete the name is.

  The base implementation handles \a day values from 1 to 7 using the day names
  CLDR provides, which are suitable for calendards that use the same
  (Hebrew-derived) week as the Gregorian calendar.

  Calendars whose dayOfWeek() returns a value outside the range from 1 to 7 need
  to reimplement this method to handle such extra week-day values. They can
  assume that \a day is a value returned by the same calendar's dayOfWeek().

  \sa dayOfWeek(), standaloneWeekDayName(), QLocale::dayName()
*/

/*!
  \fn QString QCalendarBackend::standaloneWeekDayName(const QLocale &locale, int day,
                                                      QLocale::FormatType format) const

  Returns the standalone name of the specified \a day of the week in the chosen
  \a locale, using the specified \a format to determine how complete the name
  is.

  The base implementation handles \a day values from 1 to 7 using the standalone
  day names CLDR provides, which are suitable for calendards that use the same
  (Hebrew-derived) week as the Gregorian calendar.

  Calendars whose dayOfWeek() returns a value outside the range from 1 to 7 need
  to reimplement this method to handle such extra week-day values. They can
  assume that \a day is a value returned by the same calendar's dayOfWeek().

  \sa dayOfWeek(), weekDayName(), QLocale::standaloneDayName()
*/

/*!
  \fn QString QCalendarBackend::dateTimeToString(QStringView format, const QDateTime &datetime,
                                                 const QDate &dateOnly, const QTime &timeOnly,
                                                 const QLocale &locale) const

  Returns a string representing a given date, time or date-time.

  If \a datetime is specified and valid, it is used and both date and time
  format tokens are converted to appropriate representations of the parts of the
  datetime. Otherwise, if \a dateOnly is valid, only date format tokens are
  converted; else, if \a timeOnly is valid, only time format tokens are
  converted. If none are valid, an empty string is returned.

  The specified \a locale influences how some format tokens are converted; for
  example, when substituting day and month names and their short-forms. For the
  supported formatting tokens, see QDate::toString() and QTime::toString(). As
  described above, the provided date, time and date-time determine which of
  these tokens are recognized: where these appear in \a format they are replaced
  by data. Any text in \a format not recognized as a format token is copied
  verbatim into the result string.

  \sa QDate::toString(), QTime::toString(), QDateTime::toString()
*/
// End of methods implemented in qlocale.cpp

// Registry

/*!
    Ensures each enum-available calendar has been instantiated.

    This arranges for each to register itself by name; it only does anything on
    its first call, which ensures that name-based lookups can always find all
    the calendars available via the enum.
*/
void QCalendarBackend::scanEnum()
{
    static bool todo = true;
    if (todo) {
#define SYSTEM_INSTANCE(name, ignored) QCalendar name ## Inst(QCalendar::System::name);
        FOREACH_CALENDAR(SYSTEM_INSTANCE)
#undef SYSTEM_INSTANCE

        todo = false;
    }
}

/*!
    Returns a list of names of the available calendar systems. Any
    QCalendarBackend sub-class must be registered before being exposed to Date
    and Time APIs.

    \sa registerCalendar(), fromName()
*/
QStringList QCalendarBackend::availableCalendars()
{
    scanEnum();
    return s_calendars.keys();
}

/*!
    Registers a name for this calendar backend. Once a backend is registered,
    its name will be included in the list of available calendars and the
    calendar can be instantiated by name.

    Returns \c false if the given \a name is already in use, otherwise it
    registers this calendar backend and returns \c true.

    \sa availableCalendars(), fromName()
*/
bool QCalendarBackend::registerCalendar(const QString &name)
{
    Q_ASSERT(!name.isEmpty());
    if (s_calendars.contains(name)) {
        qDebug() << "Calendar name" << name << "is already taken";
        return false;
    }

    s_calendars.insert(name, this);
    return true;
}

/*!
  Returns a pointer to a named calendar backend.

  If the given \a name is present in availableCalendars(), the backend matching
  it is returned; otherwise, \c nullptr is returned. Matching of names ignores
  case. Note that this won't provoke construction of a calendar backend, it will
  only return ones that have been instantiated (and not yet destroyed) by some
  other means. However, calendars available via the QCalendar::System enum are
  always registered when this is called.

  \sa availableCalendars(), registerCalendar(), fromEnum()
*/
const QCalendarBackend *QCalendarBackend::fromName(QStringView name)
{
    scanEnum();
    for (auto it = s_calendars.constBegin(), end = s_calendars.constEnd(); it != end; it++) {
        if (name.compare(it.key(), Qt::CaseInsensitive) == 0)
            return it.value();
    }
    return nullptr;
}

/*!
  \overload
 */
const QCalendarBackend *QCalendarBackend::fromName(QLatin1String name)
{
    scanEnum();
    for (auto it = s_calendars.constBegin(), end = s_calendars.constEnd(); it != end; it++) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0)
            return it.value();
    }
    return nullptr;
}

/*!
  Returns a pointer to a calendar backend, specified by enum.

  This will instantiate the indicated calendar (which will enable fromName() to
  return it subsequently), but only for the Qt-supported calendars for which
  (where relevant) the appropriate feature has been enabled.
*/
const QCalendarBackend *QCalendarBackend::fromEnum(QCalendar::System system)
{
    switch (system) {
#define INSTANCECASE(name, ignored) \
    case QCalendar::System::name: \
        return Q ## name ## Calendar::instance();
    FOREACH_CALENDAR(INSTANCECASE)
#undef INSTANCECASE
    case QCalendar::System::Last:
        Q_UNREACHABLE();
        break;
    }
    return nullptr;
}

/*!
  \since 5.14

  \class QCalendar
  \inmodule QtCore
  \reentrant
  \brief The QCalendar class describes calendar systems.

  A QCalendar object maps a year, month and day-number to a specific day
  (ultimately identified by its Julian day number), using the rules of a
  particular system.

  The default QCalendar() is a proleptic Gregorian calendar, which has no year
  zero. Other calendars may be supported by enabling suitable features or
  loading plugins. Calendars supported as features can be constructed by passing
  the QCalendar::System enumeration to the constructor. All supported calendars
  may be constructed by name, once they have been constructed. (Thus plugins
  instantiate their calendar backend to register it.) Built-in backends,
  accessible via QCalendar::System, are also always available by name.

  A QCalendar value is immutable.

  \sa QCalendarBackend, QDate, QDateTime
*/

/*!
    \enum QCalendar::System

    This enumerated type is used to specify a choice of calendar system.

    \value Gregorian The default calendar, used internationally.

    \sa QCalendar
*/

/*!
  \fn QCalendar::QCalendar(QCalendar::System system)
  \fn QCalendar::QCalendar(QLatin1String name)
  \fn QCalendar::QCalendar(QStringView name)

  Constructs a calendar object.

  The choice of calendar to use may be indicated as \a system, using the
  enumeration QCalendar::System, or by \a name, using a string (either Unicode
  or Latin 1). Construction by name may depend on an instance of the given
  calendar being constructed by other means first.

  \sa QCalendar, System
*/

QCalendar::QCalendar(QCalendar::System system)
    : d(QCalendarBackend::fromEnum(system)) {}

QCalendar::QCalendar(QLatin1String name)
    : d(QCalendarBackend::fromName(name)) {}

QCalendar::QCalendar(QStringView name)
    : d(QCalendarBackend::fromName(name)) {}

// Date queries:

/*!
  Returns the number of days in the given \a month of the given \a year.

  Months are numbered consecutively, starting with 1 for the first month of each
  year.

  \sa maxDaysInMonth(), minDaysInMonth()
*/
int QCalendar::daysInMonth(int month, int year) const
{
    return d ? d->daysInMonth(month, year) : 0;
}

/*!
  Returns the number of days in the given \a year.
*/
int QCalendar::daysInYear(int year) const
{
    return d ? d->daysInYear(year) : 0;
}

/*!
  Returns the number of months in the given \a year.
*/
int QCalendar::monthsInYear(int year) const
{
    return d ? d->monthsInYear(year) : 0;
}

/*!
  Returns \c true precisely if the given \a year, \a month and \a day specify a
  valid date in this calendar.

  Usually this means 1 <= month <= monthsInYear(year) and 1 <= day <=
  daysInMonth(month, year). However, calendars with intercallary days or months
  may complicate that.
*/
bool QCalendar::isValid(int year, int month, int day) const
{
    return d && d->isValid(year, month, day);
}

// properties of the calendar

/*!
    Returns \c true if this calendar object is the default calendar system used
    by Qt, e.g. in QDate; that is, it only returns true for the Gregorian
    calendar.
*/
bool QCalendar::isSystemCalendar() const
{
    return d == QGregorianCalendar::instance();
}

/*!
  Returns \c true if the given year is a leap year.

  Since the year is not a whole number of days long, some years are longer than
  others. The difference may be a whole month or just a single day; the details
  vary between calendars.

  \sa isValid()
*/
bool QCalendar::isLeapYear(int year) const
{
    return d && d->isLeapYear(year);
}

/*!
  Returns \c true if this calendar is a lunar calendar.

  A lunar calendar is one based primarily on the phases of the moon.
*/
bool QCalendar::isLunar() const
{
    return d && d->isLunar();
}

/*!
  Returns \c true if this calendar is luni-solar.

  A luni-solar calendar expresses the phases of the moon but adapts itself to
  also keep track of the Sun's varying position in the sky, relative to the
  fixed stars.
*/
bool QCalendar::isLuniSolar() const
{
    return d && d->isLuniSolar();
}

/*!
  Returns \c true if this calendar is solar.

  A solar calendar is based primaril on the Sun's varying position in the sky,
  relative to the fixed stars.
*/
bool QCalendar::isSolar() const
{
    return d && d->isSolar();
}

/*!
  Returns \c true if this calendar is proleptic.

  A proleptic calendar is able to describe years arbitrarily long before its
  first. These are represented by negative year numbers and possibly by a year
  zero.

  \sa hasYearZero()
*/
bool QCalendar::isProleptic() const
{
    return d && d->isProleptic();
}

/*!
  Returns \c true if this calendar has a year zero.

  A non-proleptic calendar with no year zero represents years from its first
  year onwards but provides no way to describe years before its first; such a
  calendar has no year zero and is not proleptic.

  A calendar which represents years before its first may number these years
  simply by following the usual integer counting, so that the year before the
  first is year zero, with negative-numbered years preceding this; such a
  calendar is proleptic and has a year zero. A calendar might also have a year
  zero (for example, the year of some great event, with subsequent years being
  the first year after that event, the second year after, and so on) without
  describing years before its year zero. Such a calendar would have a year zero
  without being proleptic.

  Some calendars, however, represent years before their first by an alternate
  numbering; for example, the proleptic Gregorian calendar's first year is 1 CE
  and the year before it is 1 BCE, preceded by 2 BCE and so on. In this case,
  we use negative year numbers, with year -1 as the year before year 1, year -2
  as the year before year -1 and so on. Such a calendar is proleptic but has no
  year zero.

  \sa isProleptic()
*/
bool QCalendar::hasYearZero() const
{
    return d && d->hasYearZero();
}

/*!
  Returns the number of days in the longest month in the calendar, in any year.

  \sa daysInMonth(), minDaysInMonth()
*/
int QCalendar::maxDaysInMonth() const
{
    return d ? d->maxDaysInMonth() : 0;
}

/*!
  Returns the number of days in the shortest month in the calendar, in any year.

  \sa daysInMonth(), maxDaysInMonth()
*/
int QCalendar::minDaysInMonth() const
{
    return d ? d->minDaysInMonth() : 0;
}

/*!
  Returns the largest number of months that any year may contain.

  \sa monthName(), standaloneMonthName(), monthsInYear()
*/
int QCalendar::maxMonthsInYear() const
{
    return d ?  d->maxMonthsInYear() : 0;
}

// Julian Day conversions:

/*!
  Converts a year, month and day to a QDate.

  Returns a QDate with the given \a year, \a month and \a day of the month in
  this calendar, if there is one. Otherwise, returns a QDate whose isNull() is
  true.

  \sa isValid(), partsFromDate()
*/
QDate QCalendar::dateFromParts(int year, int month, int day) const
{
    qint64 jd;
    return d && d->dateToJulianDay(year, month, day, jd)
        ? QDate::fromJulianDay(jd) : QDate();
}

/*!
  Converts a QDate to a year, month and day of the month.

  Returns true if the calendar is able to represent the given \a date. If so, it
  sets \a year, \a month and \a day to the so-named parts of its representation.

  \sa dateFromParts(), isProleptic(), hasYearZero()
*/
bool QCalendar::partsFromDate(QDate date, int &year, int &month, int &day) const
{
    return d && d->julianDayToDate(date.toJulianDay(), year, month, day);
}

/*!
  Returns the day of the week number for the given \a date.

  Returns zero if the calendar is unable to represent the indicated date.
  Returns 1 for Monday through 7 for Sunday. Calendars with intercallary days
  may use other numbers to represent these.

  \sa partsFromDate(), Qt::DayOfWeek
*/
int QCalendar::dayOfWeek(QDate date) const
{
    return d ? d->dayOfWeek(date.toJulianDay()) : 0;
}

// Locale data access

/*!
  Returns a suitably localised name for a month.

  The month is indicated by a number, with \a month = 1 meaning the first month
  of the year and subsequent months numbered accordingly. Returns an empty
  string if the \a month number is unrecognized.

  The \a year may be Unspecified, in which case the mapping from numbers to
  names for a typical year's months should be used. Some calendars have leap
  months that aren't always at the end of the year; their mapping of month
  numbers to names may then depend on the placement of a leap month. Thus the
  year should normally be specified, if known.

  The name is returned in the form that would normally be used in a full date,
  in the specified \a locale; the \a format determines how fully it shall be
  expressed (i.e. to what extent it is abbreviated).

  \sa standaloneMonthName(), maxMonthsInYear(), dateTimeString()
*/
QString QCalendar::monthName(const QLocale &locale, int month, int year,
                             QLocale::FormatType format) const
{
    return d ? d->monthName(locale, month, year, format) : QString();
}

/*!
  Returns a suitably localised standalone name for a month.

  The month is indicated by a number, with \a month = 1 meaning the first month
  of the year and subsequent months numbered accordingly. Returns an empty
  string if the \a month number is unrecognized.

  The \a year may be Unspecified, in which case the mapping from numbers to
  names for a typical year's months should be used. Some calendars have leap
  months that aren't always at the end of the year; their mapping of month
  numbers to names may then depend on the placement of a leap month. Thus the
  year should normally be specified, if known.

  The name is returned in the form that would be used in isolation in the
  specified \a locale; the \a format determines how fully it shall be expressed
  (i.e. to what extent it is abbreviated).

  \sa monthName(), maxMonthsInYear(), dateTimeString()
*/
QString QCalendar::standaloneMonthName(const QLocale &locale, int month, int year,
                                       QLocale::FormatType format) const
{
    return d ? d->standaloneMonthName(locale, month, year, format) : QString();
}

/*!
  Returns a suitably localised name for a day of the week.

  The days of the week are numbered from 1 for Monday through 7 for Sunday. Some
  calendars may support higher numbers for other days (e.g. intercallary days,
  that are not part of any week). Returns an empty string if the \a day number
  is unrecognized.

  The name is returned in the form that would normally be used in a full date,
  in the specified \a locale; the \a format determines how fully it shall be
  expressed (i.e. to what extent it is abbreviated).

  \sa standaloneWeekDayName(), dayOfWeek()
*/
QString QCalendar::weekDayName(const QLocale &locale, int day,
                               QLocale::FormatType format) const
{
    return d ? d->weekDayName(locale, day, format) : QString();
}

/*!
  Returns a suitably localised standalone name for a day of the week.

  The days of the week are numbered from 1 for Monday through 7 for Sunday. Some
  calendars may support higher numbers for other days (e.g. intercallary days,
  that are not part of any week). Returns an empty string if the \a day number
  is unrecognized.

  The name is returned in the form that would be used in isolation (for example
  as a column heading in a calendar's tabular display of a month with successive
  weeks as rows) in the specified \a locale; the \a format determines how fully
  it shall be expressed (i.e. to what extent it is abbreviated).

  \sa weekDayName(), dayOfWeek()
*/
QString QCalendar::standaloneWeekDayName(const QLocale &locale, int day,
                                         QLocale::FormatType format) const
{
    return d ? d->standaloneWeekDayName(locale, day, format) : QString();
}

/*!
  Returns a string representing a given date, time or date-time.

  If \a datetime is valid, it is represented and format specifiers for both date
  and time fields are recognized; otherwise, if \a dateOnly is valid, it is
  represented and only format specifiers for date fields are recognized;
  finally, if \a timeOnly is valid, it is represented and only format specifiers
  for time fields are recognized. If none of these is valid, an empty string is
  returned.

  See QDate::toString and QTime::toString() for the supported field specifiers.
  Characters in \a format that are recognized as field specifiers are replaced
  by text representing appropriate data from the date and/or time being
  represented. The texts to represent them may depend on the \a locale
  specified. Other charagers in \a format are copied verbatim into the returned
  string.

  \sa monthName(), weekDayName(), QDate::toString(), QTime::toString()
*/
QString QCalendar::dateTimeToString(QStringView format, const QDateTime &datetime,
                                    const QDate &dateOnly, const QTime &timeOnly,
                                    const QLocale &locale) const
{
    return d ? d->dateTimeToString(format, datetime, dateOnly, timeOnly, locale) : QString();
}

/*!
    Returns a list of names of the available calendar systems.

    These may be supplied by plugins or other code linked into an application,
    in addition to the ones provided by Qt, some of which are controlled by
    features.
*/
QStringList QCalendar::availableCalendars()
{
    return QCalendarBackend::availableCalendars();
}

QT_END_NAMESPACE
