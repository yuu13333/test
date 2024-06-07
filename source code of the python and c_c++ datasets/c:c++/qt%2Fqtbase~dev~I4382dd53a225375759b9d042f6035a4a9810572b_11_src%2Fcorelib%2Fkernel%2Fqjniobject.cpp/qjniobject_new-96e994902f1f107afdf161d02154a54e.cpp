/****************************************************************************
**
** Copyright (C) 2021 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtCore module of the Qt Toolkit.
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

#include "qjnienvironment.h"
#include "qjnihelpers_p.h"
#include "qjniobject.h"

#include <QtCore/QReadWriteLock>
#include <QtCore/QHash>

QT_BEGIN_NAMESPACE

/*!
    \class QJniObject
    \inmodule QtCore
    \brief Provides a convenient set of APIs to call Java code from C++ using the Java Native Interface (JNI).
    \since 6.1

    \sa QJniEnvironment

    \section1 General Notes

    \list
        \li Class names needs to contain the fully-qualified class name, for example: \b"java/lang/String".
        \li Method signatures are written as \b"(Arguments)ReturnType"
        \li All object types are returned as a QJniObject.
    \endlist

    \note This API has been tested mainly for Android.

    \section1 Method Signatures

    For functions that take no arguments, QJniObject provides convenience functions that will use
    the correct signature based on the provided template type. For example:

    \code
    jint x = QJniObject::callMethod<jint>("getSize");
    QJniObject::callMethod<void>("touch");
    \endcode

    In other cases you will need to supply the signature yourself, and it is important that the
    signature matches the function you want to call. The signature structure is \b \(A\)R, where \b A
    is the type of the argument\(s\) and \b R is the return type. Array types in the signature must
    have the \b\[ suffix and the fully-qualified type names must have the \b L prefix and \b ; suffix.

    The example below demonstrates how to call two different static functions.
    \code
    // Java class
    package org.qtproject.qt;
    class TestClass
    {
       static String fromNumber(int x) { ... }
       static String[] stringArray(String s1, String s2) { ... }
    }
    \endcode

    The signature for the first function is \b"\(I\)Ljava/lang/String;"

    \code
    // C++ code
    QJniObject stringNumber = QJniObject::callStaticObjectMethod("org/qtproject/qt/TestClass",
                                                                               "fromNumber"
                                                                               "(I)Ljava/lang/String;",
                                                                               10);
    \endcode

    and the signature for the second function is \b"\(Ljava/lang/String;Ljava/lang/String;\)\[Ljava/lang/String;"

    \code
    // C++ code
    QJniObject string1 = QJniObject::fromString("String1");
    QJniObject string2 = QJniObject::fromString("String2");
    QJniObject stringArray = QJniObject::callStaticObjectMethod("org/qtproject/qt/TestClass",
                                                                              "stringArray"
                                                                              "(Ljava/lang/String;Ljava/lang/String;)[Ljava/lang/String;"
                                                                               string1.object<jstring>(),
                                                                               string2.object<jstring>());
    \endcode


    \section1 Handling Java Exception

    When calling Java functions that might throw an exception, it is important that you check, handle
    and clear out the exception before continuing.

    \note It is unsafe to make a JNI call when there are exceptions pending.

    \snippet jni/src_qjJniobject.cpp Check for exceptions

    \section1 Java Native Methods

    Java native methods makes it possible to call native code from Java, this is done by creating a
    function declaration in Java and prefixing it with the \b native keyword.
    Before a native function can be called from Java, you need to map the Java native function to a
    native function in your code. Mapping functions can be done by calling the RegisterNatives() function
    through the \l{QJniEnvironment}{JNI environment pointer}.

    The example below demonstrates how this could be done.

    Java implementation:
    \snippet jni/src_qjniobject.cpp Java native methods

    C++ Implementation:
    \snippet jni/src_qjniobject.cpp Registering native methods

    \section1 The Lifetime of a Java Object

    Most \l{Object types}{objects} received from Java will be local references and will only stay valid
    in the scope you received them. After that, the object becomes eligible for garbage collection. If you
    want to keep a Java object alive you need to either create a new global reference to the object and
    release it when you are done, or construct a new QJniObject and let it manage the lifetime of the Java object.
    \sa object()

    \note The QJniObject does only manage its own references, if you construct a QJniObject from a
          global or local reference that reference will not be released by the QJniObject.

    \section1 JNI Types

    \section2 Object Types
    \table
    \header
        \li Type
        \li Signature
    \row
        \li jobject
        \li Ljava/lang/Object;
    \row
        \li jclass
        \li Ljava/lang/Class;
    \row
        \li jstring
        \li Ljava/lang/String;
    \row
        \li jthrowable
        \li Ljava/lang/Throwable;
    \row
        \li jobjectArray
        \li [Ljava/lang/Object;
    \row
        \li jarray
        \li [\e<type>
    \row
        \li jbooleanArray
        \li [Z
    \row
        \li jbyteArray
        \li [B
    \row
        \li jcharArray
        \li [C
    \row
        \li jshortArray
        \li [S
    \row
        \li jintArray
        \li [I
    \row
        \li jlongArray
        \li [J
    \row
        \li jfloatArray
        \li [F
    \row
        \li jdoubleArray
        \li [D
    \endtable

    \section2 Primitive Types
    \table
    \header
        \li Type
        \li Signature
    \row
        \li jboolean
        \li Z
    \row
        \li jbyte
        \li B
    \row
        \li jchar
        \li C
    \row
       \li jshort
       \li S
    \row
        \li jint
        \li I
    \row
        \li jlong
        \li J
    \row
        \li jfloat
        \li F
    \row
        \li jdouble
        \li D
    \endtable

    \section3 Other
    \table
    \header
        \li Type
        \li Signature
    \row
        \li void
        \li V
    \row
        \li \e{Custom type}
        \li L\e<fully-qualified-name>;
    \endtable

    For more information about JNI see: \l http://docs.oracle.com/javase/7/docs/technotes/guides/jni/spec/jniTOC.html
*/

/*!
    \fn bool operator==(const QJniObject &o1, const QJniObject &o2)

    \relates QJniObject

    Returns true if both objects, \a o1 and \a o2, are referencing the same Java object, or if both
    are NULL. In any other cases false will be returned.
*/

/*!
    \fn bool operator!=(const QJniObject &o1, const QJniObject &o2)
    \relates QJniObject

    Returns true if \a o1 holds a reference to a different object than \a o2.
*/

static inline QLatin1String keyBase()
{
    return QLatin1String("%1%2:%3");
}

static QString qt_convertJString(jstring string)
{
    QJniEnvironment env;
    int strLength = env->GetStringLength(string);
    QString res(strLength, Qt::Uninitialized);
    env->GetStringRegion(string, 0, strLength, reinterpret_cast<jchar *>(res.data()));
    return res;
}

typedef QHash<QString, jclass> JClassHash;
Q_GLOBAL_STATIC(JClassHash, cachedClasses)
Q_GLOBAL_STATIC(QReadWriteLock, cachedClassesLock)

static QByteArray toBinaryEncClassName(const QByteArray &className)
{
    return QByteArray(className).replace('/', '.');
}

static jclass getCachedClass(const QByteArray &classBinEnc, bool *isCached = nullptr)
{
    QReadLocker locker(cachedClassesLock);
    const QHash<QString, jclass>::const_iterator &it = cachedClasses->constFind(QString::fromLatin1(classBinEnc));
    const bool found = (it != cachedClasses->constEnd());

    if (isCached)
        *isCached = found;

    return found ? it.value() : 0;
}

inline static jclass loadClass(const QByteArray &className, JNIEnv *env, bool binEncoded = false)
{
    const QByteArray &binEncClassName = binEncoded ? className : toBinaryEncClassName(className);

    bool isCached = false;
    jclass clazz = getCachedClass(binEncClassName, &isCached);
    if (clazz || isCached)
        return clazz;

    QJniObject classLoader(QtAndroidPrivate::classLoader());
    if (!classLoader.isValid())
        return nullptr;

    QWriteLocker locker(cachedClassesLock);
    // did we lose the race?
    const QLatin1String key(binEncClassName);
    const QHash<QString, jclass>::const_iterator &it = cachedClasses->constFind(key);
    if (it != cachedClasses->constEnd())
        return it.value();

    QJniObject stringName = QJniObject::fromString(key);
    QJniObject classObject = classLoader.callObjectMethod("loadClass",
                                                          "(Ljava/lang/String;)Ljava/lang/Class;",
                                                          stringName.object());

    if (!QJniEnvironment::exceptionCheckAndClear(env) && classObject.isValid())
        clazz = static_cast<jclass>(env->NewGlobalRef(classObject.object()));

    cachedClasses->insert(key, clazz);
    return clazz;
}

typedef QHash<QString, jmethodID> JMethodIDHash;
Q_GLOBAL_STATIC(JMethodIDHash, cachedMethodID)
Q_GLOBAL_STATIC(QReadWriteLock, cachedMethodIDLock)

static inline jmethodID getMethodID(JNIEnv *env,
                                    jclass clazz,
                                    const char *name,
                                    const char *sig,
                                    bool isStatic = false)
{
    jmethodID id = isStatic ? env->GetStaticMethodID(clazz, name, sig)
                            : env->GetMethodID(clazz, name, sig);

    if (QJniEnvironment::exceptionCheckAndClear(env))
        return nullptr;

    return id;
}

static jmethodID getCachedMethodID(JNIEnv *env,
                                   jclass clazz,
                                   const QByteArray &className,
                                   const char *name,
                                   const char *sig,
                                   bool isStatic = false)
{
    if (className.isEmpty())
        return getMethodID(env, clazz, name, sig, isStatic);

    const QString key = keyBase().arg(QLatin1String(className), QLatin1String(name), QLatin1String(sig));
    QHash<QString, jmethodID>::const_iterator it;

    {
        QReadLocker locker(cachedMethodIDLock);
        it = cachedMethodID->constFind(key);
        if (it != cachedMethodID->constEnd())
            return it.value();
    }

    {
        QWriteLocker locker(cachedMethodIDLock);
        it = cachedMethodID->constFind(key);
        if (it != cachedMethodID->constEnd())
            return it.value();

        jmethodID id = getMethodID(env, clazz, name, sig, isStatic);

        cachedMethodID->insert(key, id);
        return id;
    }
}

typedef QHash<QString, jfieldID> JFieldIDHash;
Q_GLOBAL_STATIC(JFieldIDHash, cachedFieldID)
Q_GLOBAL_STATIC(QReadWriteLock, cachedFieldIDLock)

static inline jfieldID getFieldID(JNIEnv *env,
                                  jclass clazz,
                                  const char *name,
                                  const char *sig,
                                  bool isStatic = false)
{
    jfieldID id = isStatic ? env->GetStaticFieldID(clazz, name, sig)
                           : env->GetFieldID(clazz, name, sig);

    if (QJniEnvironment::exceptionCheckAndClear(env))
        return nullptr;

    return id;
}

static jfieldID getCachedFieldID(JNIEnv *env,
                                 jclass clazz,
                                 const QByteArray &className,
                                 const char *name,
                                 const char *sig,
                                 bool isStatic = false)
{
    if (className.isNull())
        return getFieldID(env, clazz, name, sig, isStatic);

    const QString key = keyBase().arg(QLatin1String(className), QLatin1String(name), QLatin1String(sig));
    QHash<QString, jfieldID>::const_iterator it;

    {
        QReadLocker locker(cachedFieldIDLock);
        it = cachedFieldID->constFind(key);
        if (it != cachedFieldID->constEnd())
            return it.value();
    }

    {
        QWriteLocker locker(cachedFieldIDLock);
        it = cachedFieldID->constFind(key);
        if (it != cachedFieldID->constEnd())
            return it.value();

        jfieldID id = getFieldID(env, clazz, name, sig, isStatic);

        cachedFieldID->insert(key, id);
        return id;
    }
}

jclass QtAndroidPrivate::findClass(const char *className, JNIEnv *env)
{
    const QByteArray &classDotEnc = toBinaryEncClassName(className);
    bool isCached = false;
    jclass clazz = getCachedClass(classDotEnc, &isCached);

    const bool found = clazz || (clazz == 0 && isCached);

    if (found)
        return clazz;

    const QLatin1String key(classDotEnc);
    if (env) { // We got an env. pointer (We expect this to be the right env. and call FindClass())
        QWriteLocker locker(cachedClassesLock);
        const QHash<QString, jclass>::const_iterator &it = cachedClasses->constFind(key);
        // Did we lose the race?
        if (it != cachedClasses->constEnd())
            return it.value();

        jclass fclazz = env->FindClass(className);
        if (!QJniEnvironment::exceptionCheckAndClear(env)) {
            clazz = static_cast<jclass>(env->NewGlobalRef(fclazz));
            env->DeleteLocalRef(fclazz);
        }

        if (clazz)
            cachedClasses->insert(key, clazz);
    }

    if (clazz == 0) // We didn't get an env. pointer or we got one with the WRONG class loader...
        clazz = loadClass(classDotEnc, QJniEnvironment(), true);

    return clazz;
}

class QJniObjectPrivate
{
public:
    QJniObjectPrivate() = default;
    ~QJniObjectPrivate() {
        QJniEnvironment env;
        if (m_jobject)
            env->DeleteGlobalRef(m_jobject);
        if (m_jclass && m_own_jclass)
            env->DeleteGlobalRef(m_jclass);
    }

    jobject m_jobject = nullptr;
    jclass m_jclass = nullptr;
    bool m_own_jclass = true;
    QByteArray m_className;
};

/*!
    \fn QJniObject::QJniObject()

    Constructs an invalid QJniObject.

    \sa isValid()
*/
QJniObject::QJniObject()
    : d(new QJniObjectPrivate())
{
}

/*!
    \fn QJniObject::QJniObject(const char *className)

    Constructs a new QJniObject by calling the default constructor of \a className.

    \code
    QJniObject myJavaString("java/lang/String");
    \endcode
*/
QJniObject::QJniObject(const char *className)
    : d(new QJniObjectPrivate())
{
    QJniEnvironment env;
    d->m_className = toBinaryEncClassName(className);
    d->m_jclass = loadClass(d->m_className, env, true);
    d->m_own_jclass = false;
    if (d->m_jclass) {
        // get default constructor
        jmethodID constructorId = getCachedMethodID(env, d->m_jclass, d->m_className, "<init>", "()V");
        if (constructorId) {
            jobject obj = env->NewObject(d->m_jclass, constructorId);
            if (obj) {
                d->m_jobject = env->NewGlobalRef(obj);
                env->DeleteLocalRef(obj);
            }
        }
    }
}

/*!
    \fn QJniObject::QJniObject(const char *className, const char *signature, ...)

    Constructs a new QJniObject by calling the constructor of \a className with \a signature
    and arguments.

    \code
    QJniEnvironment env;
    char* str = "Hello";
    jstring myJStringArg = env->NewStringUTF(str);
    QJniObject myNewJavaString("java/lang/String", "(Ljava/lang/String;)V", myJStringArg);
    \endcode
*/
QJniObject::QJniObject(const char *className, const char *sig, ...)
    : d(new QJniObjectPrivate())
{
    QJniEnvironment env;
    d->m_className = toBinaryEncClassName(className);
    d->m_jclass = loadClass(d->m_className, env, true);
    d->m_own_jclass = false;
    if (d->m_jclass) {
        jmethodID constructorId = getCachedMethodID(env, d->m_jclass, d->m_className, "<init>", sig);
        if (constructorId) {
            va_list args;
            va_start(args, sig);
            jobject obj = env->NewObjectV(d->m_jclass, constructorId, args);
            va_end(args);
            if (obj) {
                d->m_jobject = env->NewGlobalRef(obj);
                env->DeleteLocalRef(obj);
            }
        }
    }
}

QJniObject::QJniObject(const char *className, const char *sig, const QVaListPrivate &args)
    : d(new QJniObjectPrivate())
{
    QJniEnvironment env;
    d->m_className = toBinaryEncClassName(className);
    d->m_jclass = loadClass(d->m_className, env, true);
    d->m_own_jclass = false;
    if (d->m_jclass) {
        jmethodID constructorId = getCachedMethodID(env, d->m_jclass, d->m_className, "<init>", sig);
        if (constructorId) {
            jobject obj = env->NewObjectV(d->m_jclass, constructorId, args);
            if (obj) {
                d->m_jobject = env->NewGlobalRef(obj);
                env->DeleteLocalRef(obj);
            }
        }
    }
}

/*!
    \fn QJniObject::QJniObject(jclass clazz, const char *signature, ...)

    Constructs a new QJniObject from \a clazz by calling the constructor with \a signature
    and arguments.

    \code
    QJniEnvironment env;
    jclass myClazz = env.findClass("org/qtproject/qt/TestClass");
    QJniObject(myClazz, "(I)V", 3);
    \endcode
*/
QJniObject::QJniObject(jclass clazz, const char *sig, ...)
    : d(new QJniObjectPrivate())
{
    QJniEnvironment env;
    if (clazz) {
        d->m_jclass = static_cast<jclass>(env->NewGlobalRef(clazz));
        if (d->m_jclass) {
            jmethodID constructorId = getMethodID(env, d->m_jclass, "<init>", sig);
            if (constructorId) {
                va_list args;
                va_start(args, sig);
                jobject obj = env->NewObjectV(d->m_jclass, constructorId, args);
                va_end(args);
                if (obj) {
                    d->m_jobject = env->NewGlobalRef(obj);
                    env->DeleteLocalRef(obj);
                }
            }
        }
    }
}

/*!
    \fn QJniObject::QJniObject(jclass clazz)

    Constructs a new QJniObject by calling the default constructor of \a clazz.

    Note: The QJniObject will create a new reference to the class \a clazz
          and releases it again when it is destroyed. References to the class created
          outside the QJniObject needs to be managed by the caller.
*/

QJniObject::QJniObject(jclass clazz)
    : d(new QJniObjectPrivate())
{
    QJniEnvironment env;
    d->m_jclass = static_cast<jclass>(env->NewGlobalRef(clazz));
    if (d->m_jclass) {
        // get default constructor
        jmethodID constructorId = getMethodID(env, d->m_jclass, "<init>", "()V");
        if (constructorId) {
            jobject obj = env->NewObject(d->m_jclass, constructorId);
            if (obj) {
                d->m_jobject = env->NewGlobalRef(obj);
                env->DeleteLocalRef(obj);
            }
        }
    }
}

QJniObject::QJniObject(jclass clazz, const char *sig, const QVaListPrivate &args)
    : d(new QJniObjectPrivate())
{
    QJniEnvironment env;
    if (clazz) {
        d->m_jclass = static_cast<jclass>(env->NewGlobalRef(clazz));
        if (d->m_jclass) {
            jmethodID constructorId = getMethodID(env, d->m_jclass, "<init>", sig);
            if (constructorId) {
                jobject obj = env->NewObjectV(d->m_jclass, constructorId, args);
                if (obj) {
                    d->m_jobject = env->NewGlobalRef(obj);
                    env->DeleteLocalRef(obj);
                }
            }
        }
    }
}

/*!
    \fn QJniObject::QJniObject(jobject object)

    Constructs a new QJniObject around the Java object \a object.

    \note The QJniObject will hold a reference to the Java object \a object
    and release it when destroyed. Any references to the Java object \a object
    outside QJniObject needs to be managed by the caller. In most cases you
    should never call this function with a local reference unless you intend
    to manage the local reference yourself. See QJniObject::fromLocalRef()
    for converting a local reference to a QJniObject.

    \sa fromLocalRef()
*/
QJniObject::QJniObject(jobject obj)
    : d(new QJniObjectPrivate())
{
    if (!obj)
        return;

    QJniEnvironment env;
    d->m_jobject = env->NewGlobalRef(obj);
    jclass cls = env->GetObjectClass(obj);
    d->m_jclass = static_cast<jclass>(env->NewGlobalRef(cls));
    env->DeleteLocalRef(cls);
}

/*!
    \fn QJniObject::~QJniObject()

    Destroys the QJniObject and releases any references held by the QJniObject.
*/
QJniObject::~QJniObject()
{}

/*!
    \fn template <typename T> T QJniObject::object() const

    Returns the object held by the QJniObject as type T.

    \code
    QJniObject string = QJniObject::fromString("Hello, JNI");
    jstring jstring = string.object<jstring>();
    \endcode

    \note The returned object is still owned by the QJniObject. If you want to keep the object valid
    you should create a new QJniObject or make a new global reference to the object and
    free it yourself.

    \snippet jni/src_qjniobject.cpp QJniObject scope

    \code
    jobject object = jniObject.object();
    \endcode
*/
Q_CORE_EXPORT jobject QJniObject::object() const
{
    return javaObject();
}

QJniObject QJniObject::callObjectMethodV(const char *methodName,
                                         const char *sig,
                                         va_list args) const
{
    QJniEnvironment env;
    jobject res = nullptr;
    jmethodID id = getCachedMethodID(env, d->m_jclass, d->m_className, methodName, sig);
    if (id) {
        res = env->CallObjectMethodV(d->m_jobject, id, args);
        if (res && env.exceptionCheckAndClear())
            res = nullptr;
    }

    QJniObject obj(res);
    env->DeleteLocalRef(res);
    return obj;
}

QJniObject QJniObject::callStaticObjectMethodV(const char *className,
                                               const char *methodName,
                                               const char *sig,
                                               va_list args)
{
    QJniEnvironment env;
    jobject res = nullptr;
    jclass clazz = loadClass(className, env);
    if (clazz) {
        jmethodID id = getCachedMethodID(env, clazz, toBinaryEncClassName(className), methodName, sig, true);
        if (id) {
            res = env->CallStaticObjectMethodV(clazz, id, args);
            if (res && env.exceptionCheckAndClear())
                res = nullptr;
        }
    }

    QJniObject obj(res);
    env->DeleteLocalRef(res);
    return obj;
}

QJniObject QJniObject::callStaticObjectMethodV(jclass clazz,
                                               const char *methodName,
                                               const char *sig,
                                               va_list args)
{
    QJniEnvironment env;
    jobject res = nullptr;
    jmethodID id = getMethodID(env, clazz, methodName, sig, true);
    if (id) {
        res = env->CallStaticObjectMethodV(clazz, id, args);
        if (res && env.exceptionCheckAndClear())
            res = nullptr;
    }

    QJniObject obj(res);
    env->DeleteLocalRef(res);
    return obj;
}

/*!
    \fn template <typename T> T QJniObject::callMethod(const char *methodName, const char *sig, ...) const

    Calls the method \a  methodName with a signature \a sig and returns the value.

    \code
    QJniObject myJavaString = ...;
    jint index = myJavaString.callMethod<jint>("indexOf", "(I)I", 0x0051);
    \endcode

*/
template <>
Q_CORE_EXPORT void QJniObject::callMethod<void>(const char *methodName, const char *sig, ...) const
{
    QJniEnvironment env;
    jmethodID id = getCachedMethodID(env, d->m_jclass, d->m_className, methodName, sig);
    if (id) {
        va_list args;
        va_start(args, sig);
        env->CallVoidMethodV(d->m_jobject, id, args);
        va_end(args);
        env.exceptionCheckAndClear();
    }
}

/*!
    \fn template <typename T> T QJniObject::callMethod(const char *methodName) const

    Calls the method \a methodName and returns the value.

    \code
    QJniObject myJavaString = ...;
    jint size = myJavaString.callMethod<jint>("length");
    \endcode
*/
template <>
Q_CORE_EXPORT void QJniObject::callMethod<void>(const char *methodName) const
{
    callMethod<void>(methodName, "()V");
}

/*!
    \fn template <typename T> T QJniObject::callStaticMethod(const char *className, const char *methodName, const char *signature, ...)

    Calls the static method with \a methodName with \a signature on class \a className with optional arguments.

    \code
    ...
    jint a = 2;
    jint b = 4;
    jint max = QJniObject::callStaticMethod<jint>("java/lang/Math", "max", "(II)I", a, b);
    ...
    \endcode
*/
template <>
Q_CORE_EXPORT void QJniObject::callStaticMethod<void>(const char *className,
                                                      const char *methodName,
                                                      const char *sig,
                                                      ...)
{
    QJniEnvironment env;
    jclass clazz = loadClass(className, env);
    if (clazz) {
        jmethodID id = getCachedMethodID(env, clazz, toBinaryEncClassName(className),
                                         methodName, sig, true);
        if (id) {
            va_list args;
            va_start(args, sig);
            env->CallStaticVoidMethodV(clazz, id, args);
            va_end(args);
            env.exceptionCheckAndClear();
        }
    }
}

/*!
    \fn template <typename T> T QJniObject::callStaticMethod(const char *className, const char *methodName)

    Calls the static method \a methodName on class \a className and returns the value.

    \code
    jint value = QJniObject::callStaticMethod<jint>("MyClass", "staticMethod");
    \endcode
*/
template <>
Q_CORE_EXPORT void QJniObject::callStaticMethod<void>(const char *className, const char *methodName)
{
    callStaticMethod<void>(className, methodName, "()V");
}

/*!
    \fn template <typename T> T QJniObject::callStaticMethod(jclass clazz, const char *methodName, const char *signature, ...)

    Calls the static method \a methodName with \a signature on \a clazz and returns the value.

    \code
    ...
    jclass javaMathClass = ...; // ("java/lang/Math")
    jint a = 2;
    jint b = 4;
    jint max = QJniObject::callStaticMethod<jint>(javaMathClass, "max", "(II)I", a, b);
    ...
    \endcode
*/
template <>
Q_CORE_EXPORT void QJniObject::callStaticMethod<void>(jclass clazz,
                                                      const char *methodName,
                                                      const char *sig,
                                                      ...)
{
    QJniEnvironment env;
    if (clazz) {
        jmethodID id = getMethodID(env, clazz, methodName, sig, true);
        if (id) {
            va_list args;
            va_start(args, sig);
            env->CallStaticVoidMethodV(clazz, id, args);
            va_end(args);
            env.exceptionCheckAndClear();
        }
    }
}

template <>
Q_CORE_EXPORT void QJniObject::callStaticMethodV<void>(const char *className,
                                                       const char *methodName,
                                                       const char *sig,
                                                       va_list args)
{
    QJniEnvironment env;
    jclass clazz = loadClass(className, env);
    if (clazz) {
        jmethodID id = getCachedMethodID(env, clazz,
                                         toBinaryEncClassName(className), methodName,
                                         sig, true);
        if (id) {
            env->CallStaticVoidMethodV(clazz, id, args);
            env.exceptionCheckAndClear();
        }
    }
}

template <>
Q_CORE_EXPORT void QJniObject::callStaticMethodV<void>(jclass clazz,
                                                       const char *methodName,
                                                       const char *sig,
                                                       va_list args)
{
    QJniEnvironment env;
    jmethodID id = getMethodID(env, clazz, methodName, sig, true);
    if (id) {
        env->CallStaticVoidMethodV(clazz, id, args);
        env.exceptionCheckAndClear();
    }
}

/*!
    \fn template <typename T> T QJniObject::callStaticMethod(jclass clazz, const char *methodName)

    Calls the static method \a methodName on \a clazz and returns the value.

    \code
    ...
    jclass javaMathClass = ...; // ("java/lang/Math")
    jdouble randNr = QJniObject::callStaticMethod<jdouble>(javaMathClass, "random");
    ...
    \endcode
*/
template <>
Q_CORE_EXPORT void QJniObject::callStaticMethod<void>(jclass clazz, const char *methodName)
{
    callStaticMethod<void>(clazz, methodName, "()V");
}

template <>
Q_CORE_EXPORT void QJniObject::callMethodV<void>(const char *methodName, const char *sig,
                                                 va_list args) const
{
    QJniEnvironment env;
    jmethodID id = getCachedMethodID(env, d->m_jclass, d->m_className, methodName, sig);
    if (id) {
        env->CallVoidMethodV(d->m_jobject, id, args);
        env.exceptionCheckAndClear();
    }
}

#define MAKE_JNI_METHODS(MethodName, Type, Signature) \
template <> Q_CORE_EXPORT Type QJniObject::callMethod<Type>(const char *methodName, \
                                                                   const char *sig, ...) const \
{ \
    QJniEnvironment env; \
    Type res = 0; \
    jmethodID id = getCachedMethodID(env, d->m_jclass, d->m_className, methodName, sig); \
    if (id) { \
        va_list args; \
        va_start(args, sig); \
        res = env->Call##MethodName##MethodV(d->m_jobject, id, args); \
        va_end(args); \
        if (env.exceptionCheckAndClear()) \
            res = 0; \
    } \
    return res; \
}\
template <> Q_CORE_EXPORT Type QJniObject::callMethod<Type>(const char *methodName) const \
{ \
    return callMethod<Type>(methodName, Signature); \
} \
\
template <> Q_CORE_EXPORT Type QJniObject::callStaticMethod<Type>(const char *className, \
                                                                  const char *methodName, \
                                                                  const char *sig, \
                                                                  ...) \
{ \
    QJniEnvironment env; \
    Type res = 0; \
    jclass clazz = loadClass(className, env); \
    if (clazz) { \
        jmethodID id = getCachedMethodID(env, clazz, toBinaryEncClassName(className), methodName, \
                                         sig, true); \
        if (id) { \
            va_list args; \
            va_start(args, sig); \
            res = env->CallStatic##MethodName##MethodV(clazz, id, args); \
            va_end(args); \
            if (env.exceptionCheckAndClear())  \
                res = 0; \
        } \
    } \
    return res; \
} \
template <> Q_CORE_EXPORT Type QJniObject::callStaticMethod<Type>(const char *className, \
                                                                  const char *methodName) \
{ \
    return callStaticMethod<Type>(className, methodName, Signature); \
}\
\
template <> Q_CORE_EXPORT Type QJniObject::callStaticMethod<Type>(jclass clazz, \
                                                                  const char *methodName, \
                                                                  const char *sig, \
                                                                  ...) \
{ \
    QJniEnvironment env; \
    Type res = 0; \
    if (clazz) { \
        jmethodID id = getMethodID(env, clazz, methodName, sig, true); \
        if (id) { \
            va_list args; \
            va_start(args, sig); \
            res = env->CallStatic##MethodName##MethodV(clazz, id, args); \
            va_end(args); \
            if (env.exceptionCheckAndClear()) \
                res = 0; \
        } \
    } \
    return res; \
} \
template <> Q_CORE_EXPORT Type QJniObject::callStaticMethod<Type>(jclass clazz, \
                                                                  const char *methodName) \
{ \
    return callStaticMethod<Type>(clazz, methodName, Signature); \
}\
template <> \
Q_CORE_EXPORT Type QJniObject::callMethodV<Type>(const char *methodName, const char *sig,\
                                                 va_list args) const\
{\
    QJniEnvironment env;\
    Type res = 0;\
    jmethodID id = getCachedMethodID(env, d->m_jclass, d->m_className, methodName, sig);\
    if (id) {\
        res = env->Call##MethodName##MethodV(d->m_jobject, id, args);\
        if (env.exceptionCheckAndClear())  \
            res = 0; \
    }\
    return res;\
}\
template <>\
Q_CORE_EXPORT Type QJniObject::callStaticMethodV<Type>(const char *className,\
                                                     const char *methodName,\
                                                     const char *sig,\
                                                     va_list args)\
{\
    QJniEnvironment env;\
    Type res = 0;\
    jclass clazz = loadClass(className, env);\
    if (clazz) {\
        jmethodID id = getCachedMethodID(env, clazz, toBinaryEncClassName(className), methodName,\
                                         sig, true);\
        if (id) {\
            res = env->CallStatic##MethodName##MethodV(clazz, id, args);\
            if (env.exceptionCheckAndClear())  \
                res = 0; \
        }\
    }\
    return res;\
}\
template <>\
Q_CORE_EXPORT Type QJniObject::callStaticMethodV<Type>(jclass clazz,\
                                                     const char *methodName,\
                                                     const char *sig,\
                                                     va_list args)\
{\
    QJniEnvironment env;\
    Type res = 0;\
    jmethodID id = getMethodID(env, clazz, methodName, sig, true);\
    if (id) {\
        res = env->CallStatic##MethodName##MethodV(clazz, id, args);\
        if (env.exceptionCheckAndClear())  \
            res = 0; \
    }\
    return res;\
}

#define DECLARE_JNI_METHODS(MethodName, Type, Signature) MAKE_JNI_METHODS(MethodName, \
                                                                          Type, \
                                                                          Signature)
DECLARE_JNI_METHODS(Boolean, jboolean, "()Z")
DECLARE_JNI_METHODS(Byte, jbyte, "()B")
DECLARE_JNI_METHODS(Char, jchar, "()C")
DECLARE_JNI_METHODS(Short, jshort, "()S")
DECLARE_JNI_METHODS(Int, jint, "()I")
DECLARE_JNI_METHODS(Long, jlong, "()J")
DECLARE_JNI_METHODS(Float, jfloat, "()F")
DECLARE_JNI_METHODS(Double, jdouble, "()D")

/*!
    \fn QJniObject QJniObject::callObjectMethod(const char *methodName, const char *signature, ...) const

    Calls the Java object's method \a methodName with the signature \a signature and arguments

    \code
    QJniObject myJavaString; ==> "Hello, Java"
    QJniObject mySubstring = myJavaString.callObjectMethod("substring", "(II)Ljava/lang/String;", 7, 10);
    \endcode
*/
QJniObject QJniObject::callObjectMethod(const char *methodName, const char *sig, ...) const
{
    QJniEnvironment env;
    jobject res = nullptr;
    jmethodID id = getCachedMethodID(env, d->m_jclass, d->m_className, methodName, sig);
    if (id) {
        va_list args;
        va_start(args, sig);
        res = env->CallObjectMethodV(d->m_jobject, id, args);
        va_end(args);
        if (res && env.exceptionCheckAndClear())
            res = nullptr;
    }

    QJniObject obj(res);
    env->DeleteLocalRef(res);
    return obj;
}

/*!
    \fn QJniObject QJniObject::callStaticObjectMethod(const char *className, const char *methodName, const char *signature, ...)

    Calls the static method with \a methodName and \a signature on the class \a className.

    \code
    QJniObject thread = QJniObject::callStaticObjectMethod("java/lang/Thread", "currentThread", "()Ljava/lang/Thread;");
    QJniObject string = QJniObject::callStaticObjectMethod("java/lang/String", "valueOf", "(I)Ljava/lang/String;", 10);
    \endcode
*/
QJniObject QJniObject::callStaticObjectMethod(const char *className,
                                              const char *methodName,
                                              const char *sig,
                                              ...)
{
    QJniEnvironment env;
    jobject res = nullptr;
    jclass clazz = loadClass(className, env);
    if (clazz) {
        jmethodID id = getCachedMethodID(env, clazz, toBinaryEncClassName(className), methodName,
                                         sig, true);
        if (id) {
            va_list args;
            va_start(args, sig);
            res = env->CallStaticObjectMethodV(clazz, id, args);
            va_end(args);
            if (res && env.exceptionCheckAndClear())
                res = nullptr;
        }
    }

    QJniObject obj(res);
    env->DeleteLocalRef(res);
    return obj;
}

/*!
    \fn QJniObject QJniObject::callStaticObjectMethod(jclass clazz, const char *methodName, const char *signature, ...)

    Calls the static method with \a methodName and \a signature on class \a clazz.
*/
QJniObject QJniObject::callStaticObjectMethod(jclass clazz,
                                              const char *methodName,
                                              const char *sig,
                                              ...)
{
    QJniEnvironment env;
    jobject res = nullptr;
    if (clazz) {
        jmethodID id = getMethodID(env, clazz, methodName, sig, true);
        if (id) {
            va_list args;
            va_start(args, sig);
            res = env->CallStaticObjectMethodV(clazz, id, args);
            va_end(args);
            if (res && env.exceptionCheckAndClear())
                res = nullptr;
        }
    }
    QJniObject obj(res);
    env->DeleteLocalRef(res);
    return obj;
}

/*!
    \fn QJniObject QJniObject::callObjectMethod(const char *methodName) const

    Calls the Java objects method \a methodName and returns a new QJniObject for
    the returned Java object.

    \code
    ...
    QJniObject myJavaString1 = ...;
    QJniObject myJavaString2 = myJavaString1.callObjectMethod<jstring>("toString");
    ...
    \endcode
*/

/*!
    \fn QJniObject QJniObject::callStaticObjectMethod(const char *className, const char *methodName)

    Calls the static method with \a methodName on the class \a className.

    \code
    QJniObject string = QJniObject::callStaticObjectMethod<jstring>("CustomClass", "getClassName");
    \endcode
*/

/*!
    \fn QJniObject QJniObject::callStaticObjectMethod(jclass clazz, const char *methodName)

    Calls the static method with \a methodName on \a clazz.

*/

/*!
    \fn template <typename T> QJniObject &QJniObject::operator=(T object)

    Replace the current object with \a object. The old Java object will be released.
*/
#define MAKE_JNI_OBJECT_METHODS(Type, Signature) \
template <> \
Q_CORE_EXPORT QJniObject QJniObject::callObjectMethod<Type>(const char *methodName) const \
{ \
    return callObjectMethod(methodName, Signature); \
} \
template <> \
Q_CORE_EXPORT QJniObject QJniObject::callStaticObjectMethod<Type>(const char *className, \
                                                                  const char *methodName) \
{ \
    return callStaticObjectMethod(className, methodName, Signature); \
} \
template <> \
Q_CORE_EXPORT QJniObject QJniObject::callStaticObjectMethod<Type>(jclass clazz, \
                                                                  const char *methodName) \
{ \
    return callStaticObjectMethod(clazz, methodName, Signature); \
}\
template <>\
Q_CORE_EXPORT Type QJniObject::object<Type>() const\
{\
    return static_cast<Type>(javaObject());\
}\
template <>\
Q_CORE_EXPORT QJniObject &QJniObject::operator=(Type obj)\
{\
    assign(static_cast<jobject>(obj));\
    return *this;\
}

#define DECLARE_JNI_OBJECT_METHODS(Type, Signature) MAKE_JNI_OBJECT_METHODS(Type, Signature)

DECLARE_JNI_OBJECT_METHODS(jobject, "()Ljava/lang/Object;")
DECLARE_JNI_OBJECT_METHODS(jclass, "()Ljava/lang/Class;")
DECLARE_JNI_OBJECT_METHODS(jstring, "()Ljava/lang/String;")
DECLARE_JNI_OBJECT_METHODS(jobjectArray, "()[Ljava/lang/Object;")
DECLARE_JNI_OBJECT_METHODS(jbooleanArray, "()[Z")
DECLARE_JNI_OBJECT_METHODS(jbyteArray, "()[B")
DECLARE_JNI_OBJECT_METHODS(jshortArray, "()[S")
DECLARE_JNI_OBJECT_METHODS(jintArray, "()[I")
DECLARE_JNI_OBJECT_METHODS(jlongArray, "()[J")
DECLARE_JNI_OBJECT_METHODS(jfloatArray, "()[F")
DECLARE_JNI_OBJECT_METHODS(jdoubleArray, "()[D")
DECLARE_JNI_OBJECT_METHODS(jcharArray, "()[C")
DECLARE_JNI_OBJECT_METHODS(jthrowable, "()Ljava/lang/Throwable;")

/*!
    \fn template <typename T> void QJniObject::setStaticField(const char *className, const char *fieldName, const char *signature, T value);

    Sets the static field with \a fieldName and \a signature to \a value on class named \a className.
*/
template <>
Q_CORE_EXPORT void QJniObject::setStaticField<jobject>(const char *className,
                                                       const char *fieldName,
                                                       const char *sig,
                                                       jobject value)
{
    QJniEnvironment env;
    jclass clazz = loadClass(className, env);

    if (!clazz)
        return;

    jfieldID id = getCachedFieldID(env, clazz, className, fieldName, sig, true);
    if (id) {
        env->SetStaticObjectField(clazz, id, value);
        env.exceptionCheckAndClear();
    }
}

/*!
    \fn template <typename T> void QJniObject::setStaticField(jclass clazz, const char *fieldName, const char *signature, T value);

    Sets the static field with \a fieldName and \a signature to \a value on class \a clazz.
*/
template <> Q_CORE_EXPORT void QJniObject::setStaticField<jobject>(jclass clazz,
                                                                   const char *fieldName,
                                                                   const char *sig,
                                                                   jobject value)
{
    QJniEnvironment env;
    jfieldID id = getFieldID(env, clazz, fieldName, sig, true);

    if (id) {
        env->SetStaticObjectField(clazz, id, value);
        env.exceptionCheckAndClear();
    }
}

/*!
    \fn T QJniObject::getField(const char *fieldName) const

    Retrieves the value of the field \a fieldName.

    \code
    QJniObject volumeControl = ...;
    jint fieldValue = volumeControl.getField<jint>("MAX_VOLUME");
    \endcode
*/

/*!
    \fn T QJniObject::getStaticField(const char *className, const char *fieldName)

    Retrieves the value from the static field \a fieldName on the class \a className.
*/

/*!
    \fn T QJniObject::getStaticField(jclass clazz, const char *fieldName)

    Retrieves the value from the static field \a fieldName on \a clazz.
*/

/*!
    \fn template <typename T> void QJniObject::setStaticField(const char *className, const char *fieldName, T value)

    Sets the static field \a fieldName of the class named \a className to \a value.
*/

/*!
    \fn template <typename T> void QJniObject::setStaticField(jclass clazz, const char *fieldName, T value)

    Sets the static field \a fieldName of the class \a clazz to \a value.
*/
#define MAKE_JNI_PRIMITIVE_FIELDS(FieldName, Type, Signature) \
template <> Q_CORE_EXPORT Type QJniObject::getField<Type>(const char *fieldName) const \
{ \
    QJniEnvironment env; \
    Type res = 0; \
    jfieldID id = getCachedFieldID(env, d->m_jclass, d->m_className, fieldName, Signature); \
    if (id) {\
        res = env->Get##FieldName##Field(d->m_jobject, id); \
        if (env.exceptionCheckAndClear())  \
            res = 0; \
    } \
    return res;\
} \
template <> \
Q_CORE_EXPORT Type QJniObject::getStaticField<Type>(const char *className, const char *fieldName) \
{ \
    QJniEnvironment env; \
    jclass clazz = loadClass(className, env); \
    if (!clazz) \
        return 0; \
    jfieldID id = getCachedFieldID(env, clazz, toBinaryEncClassName(className), fieldName, \
                                   Signature, true); \
    if (!id) \
        return 0; \
    Type res = env->GetStatic##FieldName##Field(clazz, id); \
    if (env.exceptionCheckAndClear())  \
        res = 0; \
    return res;\
} \
template <>\
Q_CORE_EXPORT Type QJniObject::getStaticField<Type>(jclass clazz, const char *fieldName)\
{\
    QJniEnvironment env;\
    Type res = 0;\
    jfieldID id = getFieldID(env, clazz, fieldName, Signature, true);\
    if (id) {\
        res = env->GetStatic##FieldName##Field(clazz, id);\
        if (env.exceptionCheckAndClear())  \
            res = 0; \
    }\
    return res;\
}\
template <> Q_CORE_EXPORT void QJniObject::setStaticField<Type>(const char *className, \
                                                                const char *fieldName, \
                                                                Type value) \
{ \
    QJniEnvironment env; \
    jclass clazz = loadClass(className, env); \
    if (!clazz) \
        return; \
    jfieldID id = getCachedFieldID(env, clazz, className, fieldName, Signature, true); \
    if (!id) \
        return; \
    env->SetStatic##FieldName##Field(clazz, id, value); \
    env.exceptionCheckAndClear(); \
}\
template <> Q_CORE_EXPORT void QJniObject::setStaticField<Type>(jclass clazz,\
                                                                const char *fieldName,\
                                                                Type value)\
{\
    QJniEnvironment env;\
    jfieldID id = getFieldID(env, clazz, fieldName, Signature, true);\
    if (id) {\
        env->SetStatic##FieldName##Field(clazz, id, value);\
        env.exceptionCheckAndClear();\
    }\
}\
template <> Q_CORE_EXPORT void QJniObject::setField<Type>(const char *fieldName, Type value) \
{ \
    QJniEnvironment env; \
    jfieldID id = getCachedFieldID(env, d->m_jclass, d->m_className, fieldName, Signature); \
    if (id) { \
        env->Set##FieldName##Field(d->m_jobject, id, value); \
        env.exceptionCheckAndClear(); \
    } \
} \

#define DECLARE_JNI_PRIMITIVE_FIELDS(FieldName, Type, Signature) MAKE_JNI_PRIMITIVE_FIELDS(FieldName, Type, \
                                                                               Signature)
DECLARE_JNI_PRIMITIVE_FIELDS(Boolean, jboolean, "Z")
DECLARE_JNI_PRIMITIVE_FIELDS(Byte, jbyte, "B")
DECLARE_JNI_PRIMITIVE_FIELDS(Char, jchar, "C")
DECLARE_JNI_PRIMITIVE_FIELDS(Short, jshort, "S")
DECLARE_JNI_PRIMITIVE_FIELDS(Int, jint, "I")
DECLARE_JNI_PRIMITIVE_FIELDS(Long, jlong, "J")
DECLARE_JNI_PRIMITIVE_FIELDS(Float, jfloat, "F")
DECLARE_JNI_PRIMITIVE_FIELDS(Double, jdouble, "D")

/*!
    \fn QJniObject QJniObject::getStaticObjectField(const char *className, const char *fieldName, const char *signature)
    Retrieves the object from the field with \a signature and \a fieldName on class \a className.

    \note This function can be used without a template type.

    \code
    QJniObject jobj = QJniObject::getStaticObjectField("class/with/Fields", "FIELD_NAME", "Ljava/lang/String;");
    \endcode
*/
QJniObject QJniObject::getStaticObjectField(const char *className,
                                            const char *fieldName,
                                            const char *sig)
{
    QJniEnvironment env;
    jclass clazz = loadClass(className, env);
    if (!clazz)
        return QJniObject();
    jfieldID id = getCachedFieldID(env, clazz, toBinaryEncClassName(className), fieldName,
                                   sig, true);
    if (!id)
        return QJniObject();
    jobject res = env->GetStaticObjectField(clazz, id);
    if (res && env.exceptionCheckAndClear())
        res = nullptr;
    QJniObject obj(res);
    env->DeleteLocalRef(res);
    return obj;
}

/*!
    \fn QJniObject QJniObject::getStaticObjectField(jclass clazz, const char *fieldName, const char *signature)
    Retrieves the object from the field with \a signature and \a fieldName on \a clazz.

    \note This function can be used without a template type.

    \code
    QJniObject jobj = QJniObject::getStaticObjectField(clazz, "FIELD_NAME", "Ljava/lang/String;");
    \endcode
*/
QJniObject QJniObject::getStaticObjectField(jclass clazz,
                                            const char *fieldName,
                                            const char *sig)
{
    QJniEnvironment env;
    jobject res = nullptr;
    jfieldID id = getFieldID(env, clazz, fieldName, sig, true);
    if (id) {
        res = env->GetStaticObjectField(clazz, id);
        if (res && env.exceptionCheckAndClear())
            res = nullptr;
    }

    QJniObject obj(res);
    env->DeleteLocalRef(res);
    return obj;
}

/*!
    \fn QJniObject QJniObject::getStaticObjectField<jobject>(jclass clazz, const char *fieldName, const char *signature)

    Retrieves the \a jobject from the field with \a signature and \a fieldName on \a clazz.
*/
template <>
Q_CORE_EXPORT QJniObject QJniObject::getStaticObjectField<jobject>(jclass clazz,
                                                                   const char *fieldName,
                                                                   const char *sig)
{
    return getStaticObjectField(clazz, fieldName, sig);
}

/*!
    \fn QJniObject QJniObject::getStaticObjectField<jobject>(jclass clazz, const char *fieldName, const char *signature)

    Retrieves the jobject from the field with \a signature and \a fieldName on class named \a className.
*/
template <>
Q_CORE_EXPORT QJniObject QJniObject::getStaticObjectField<jobject>(const char *className,
                                                                   const char *fieldName,
                                                                   const char *sig)
{
    return getStaticObjectField(className, fieldName, sig);
}

/*!
    \fn QJniObject QJniObject::getStaticObjectField<jobjectArray>(jclass clazz, const char *fieldName, const char *signature)

    Retrieves the jobjectArray from the field with \a signature and \a fieldName on \a clazz.
*/
template <>
Q_CORE_EXPORT QJniObject QJniObject::getStaticObjectField<jobjectArray>(jclass clazz,
                                                                        const char *fieldName,
                                                                        const char *sig)
{
    return getStaticObjectField(clazz, fieldName, sig);
}

/*!
    \fn QJniObject QJniObject::getStaticObjectField<jobjectArray>(jclass clazz, const char *fieldName, const char *signature)

    Retrieves the jobjectArray from the field with \a signature and \a fieldName on the class named \a className.
*/
template <>
Q_CORE_EXPORT QJniObject QJniObject::getStaticObjectField<jobjectArray>(const char *className,
                                                                        const char *fieldName,
                                                                        const char *sig)
{
    return getStaticObjectField(className, fieldName, sig);
}

/*!
    \fn template <typename T> void QJniObject::setField(const char *fieldName, const char *signature, T value)

    Sets the value of \a fieldName with \a signature to \a value.

    \code
    QJniObject stringArray = ...;
    QJniObject obj = ...;
    obj.setField<jobjectArray>("KEY_VALUES", "([Ljava/lang/String;)V", stringArray.object<jobjectArray>())
    \endcode
*/
template <> Q_CORE_EXPORT
void QJniObject::setField<jobject>(const char *fieldName, const char *sig, jobject value)
{
    QJniEnvironment env;
    jfieldID id = getCachedFieldID(env, d->m_jclass, d->m_className, fieldName, sig);
    if (id) {
        env->SetObjectField(d->m_jobject, id, value);
        env.exceptionCheckAndClear();
    }
}

template <> Q_CORE_EXPORT
void QJniObject::setField<jobjectArray>(const char *fieldName, const char *sig, jobjectArray value)
{
    QJniEnvironment env;
    jfieldID id = getCachedFieldID(env, d->m_jclass, d->m_className, fieldName, sig);
    if (id) {
        env->SetObjectField(d->m_jobject, id, value);
        env.exceptionCheckAndClear();
    }
}

/*!
    \fn QJniObject QJniObject::getObjectField(const char *fieldName) const

    Retrieves the object of field \a fieldName.

    \code
    QJniObject field = jniObject.getObjectField<jstring>("FIELD_NAME");
    \endcode
*/

/*!
    \fn QJniObject QJniObject::getObjectField(const char *fieldName, const char *signature) const

    Retrieves the object from the field with \a signature and \a fieldName.

    \note This function can be used without a template type.

    \code
    QJniObject field = jniObject.getObjectField("FIELD_NAME", "Ljava/lang/String;");
    \endcode
*/
QJniObject QJniObject::getObjectField(const char *fieldName, const char *sig) const
{
    QJniEnvironment env;
    jobject res = nullptr;
    jfieldID id = getCachedFieldID(env, d->m_jclass, d->m_className, fieldName, sig);
    if (id) {
        res = env->GetObjectField(d->m_jobject, id);
        if (res && env.exceptionCheckAndClear())
            res = nullptr;
    }

    QJniObject obj(res);
    env->DeleteLocalRef(res);
    return obj;
}

/*!
    \fn template <typename T> void QJniObject::setField(const char *fieldName, T value)

    Sets the value of \a fieldName to \a value.

    \code
    ...
    QJniObject obj;
    obj.setField<jint>("AN_INT_FIELD", 10);
    jstring myString = ...
    obj.setField<jstring>("A_STRING_FIELD", myString);
    ...
    \endcode
*/

/*!
    \fn QJniObject QJniObject::getStaticObjectField(const char *className, const char *fieldName)

    Retrieves the object from the field \a fieldName on the class \a className.

    \code
    QJniObject jobj = QJniObject::getStaticObjectField<jstring>("class/with/Fields", "FIELD_NAME");
    \endcode
*/

/*!
    \fn QJniObject QJniObject::getStaticObjectField(jclass clazz, const char *fieldName)

    Retrieves the object from the field \a fieldName on \a clazz.

    \code
    QJniObject jobj = QJniObject::getStaticObjectField<jstring>(clazz, "FIELD_NAME");
    \endcode
*/

#define MAKE_JNI_OBJECT_FILEDS(Type, Signature) \
template <> Q_CORE_EXPORT void QJniObject::setField<Type>(const char *fieldName, Type value) \
{ \
    QJniObject::setField<jobject>(fieldName, Signature, value); \
} \
\
template <> Q_CORE_EXPORT void QJniObject::setStaticField<Type>(const char *className, \
                                                                const char *fieldName, \
                                                                Type value) \
{ \
    QJniObject::setStaticField<jobject>(className, fieldName, Signature, value); \
}\
template <>\
Q_CORE_EXPORT QJniObject QJniObject::getObjectField<Type>(const char *fieldName) const\
{\
    return getObjectField(fieldName, Signature);\
}\
template <>\
Q_CORE_EXPORT QJniObject QJniObject::getStaticObjectField<Type>(jclass clazz,\
                                                                const char *fieldName)\
{\
    return getStaticObjectField(clazz, fieldName, Signature);\
}\
template <>\
Q_CORE_EXPORT QJniObject QJniObject::getStaticObjectField<Type>(const char *className,\
                                                                const char *fieldName)\
{\
    return getStaticObjectField(className, fieldName, Signature);\
}\

#define DECLARE_JNI_OBJECT_FILEDS(Type, Signature) MAKE_JNI_OBJECT_FILEDS(Type, Signature)

DECLARE_JNI_OBJECT_FILEDS(jobject, "Ljava/lang/Object;")
DECLARE_JNI_OBJECT_FILEDS(jobjectArray, "[Ljava/lang/Object;")
DECLARE_JNI_OBJECT_FILEDS(jstring, "Ljava/lang/String;")
DECLARE_JNI_OBJECT_FILEDS(jclass, "Ljava/lang/Class;")
DECLARE_JNI_OBJECT_FILEDS(jthrowable, "Ljava/lang/Throwable;")
DECLARE_JNI_OBJECT_FILEDS(jbooleanArray, "[Z")
DECLARE_JNI_OBJECT_FILEDS(jbyteArray, "[B")
DECLARE_JNI_OBJECT_FILEDS(jcharArray, "[C")
DECLARE_JNI_OBJECT_FILEDS(jshortArray, "[S")
DECLARE_JNI_OBJECT_FILEDS(jintArray, "[I")
DECLARE_JNI_OBJECT_FILEDS(jlongArray, "[J")
DECLARE_JNI_OBJECT_FILEDS(jfloatArray, "[F")
DECLARE_JNI_OBJECT_FILEDS(jdoubleArray, "[D")

/*!
    \fn QJniObject QJniObject::fromString(const QString &string)

    Creates a Java string from the QString \a string and returns a QJniObject holding that string.

    \code
    QString myQString = "QString";
    QJniObject myJavaString = QJniObject::fromString(myQString);
    \endcode

    \sa toString()
*/
QJniObject QJniObject::fromString(const QString &string)
{
    QJniEnvironment env;
    jstring res = env->NewString(reinterpret_cast<const jchar*>(string.constData()),
                                                                string.length());
    QJniObject obj(res);
    env->DeleteLocalRef(res);
    return obj;
}

/*!
    \fn QString QJniObject::toString() const

    Returns a QString with a string representation of the java object.
    Calling this function on a Java String object is a convenient way of getting the actual string
    data.

    \code
    QJniObject string = ...; //  "Hello Java"
    QString qstring = string.toString(); // "Hello Java"
    \endcode

    \sa fromString()
*/
QString QJniObject::toString() const
{
    if (!isValid())
        return QString();

    QJniObject string = callObjectMethod<jstring>("toString");
    return qt_convertJString(static_cast<jstring>(string.object()));
}

/*!
    \fn bool QJniObject::isClassAvailable(const char *className)

    Returns true if the Java class \a className is available.

    \code
    if (QJniObject::isClassAvailable("java/lang/String")) {
        // condition statement
    }
    \endcode
*/
bool QJniObject::isClassAvailable(const char *className)
{
    QJniEnvironment env;

    if (!env)
        return false;

    jclass clazz = loadClass(className, env);
    return (clazz != 0);
}

/*!
    \fn bool QJniObject::isValid() const

    Returns true if this instance holds a valid Java object.

    \code
    QJniObject qjniObject;                        ==> isValid() == false
    QJniObject qjniObject(0)                      ==> isValid() == false
    QJniObject qjniObject("could/not/find/Class") ==> isValid() == false
    \endcode
*/
bool QJniObject::isValid() const
{
    return d->m_jobject;
}

/*!
    \fn QJniObject QJniObject::fromLocalRef(jobject localRef)
    \since 6.1

    Creates a QJniObject from the local JNI reference \a localRef.
    This function takes ownership of \a localRef and frees it before returning.

    \note Only call this function with a local JNI reference. For example, most raw JNI calls, through
    the JNI environment, returns local references to a java object.

    \code
    jobject localRef = env->GetObjectArrayElement(array, index);
    QJniObject element = QJniObject::fromLocalRef(localRef);
    \endcode
*/
QJniObject QJniObject::fromLocalRef(jobject lref)
{
    QJniObject obj(lref);
    QJniEnvironment()->DeleteLocalRef(lref);
    return obj;
}

bool QJniObject::isSameObject(jobject obj) const
{
    return QJniEnvironment()->IsSameObject(d->m_jobject, obj);
}

bool QJniObject::isSameObject(const QJniObject &other) const
{
    return isSameObject(other.d->m_jobject);
}

void QJniObject::assign(jobject obj)
{
    if (isSameObject(obj))
        return;

    jobject jobj = static_cast<jobject>(obj);
    d = QSharedPointer<QJniObjectPrivate>::create();
    if (obj) {
        QJniEnvironment env;
        d->m_jobject = env->NewGlobalRef(jobj);
        jclass objectClass = env->GetObjectClass(jobj);
        d->m_jclass = static_cast<jclass>(env->NewGlobalRef(objectClass));
        env->DeleteLocalRef(objectClass);
    }
}

jobject QJniObject::javaObject() const
{
    return d->m_jobject;
}

QT_END_NAMESPACE
