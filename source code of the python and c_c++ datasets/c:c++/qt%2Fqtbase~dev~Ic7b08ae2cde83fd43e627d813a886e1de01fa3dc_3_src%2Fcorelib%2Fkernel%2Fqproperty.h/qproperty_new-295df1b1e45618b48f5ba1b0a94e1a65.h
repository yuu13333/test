/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
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

#ifndef QPROPERTY_H
#define QPROPERTY_H

#include <QtCore/qglobal.h>
#include <QtCore/QSharedDataPointer>
#include <QtCore/QString>
#include <functional>
#include <type_traits>
#include <variant>

#include <QtCore/qpropertyprivate.h>

#if __has_include(<source_location>) && __cplusplus >= 202002L && !defined(Q_CLANG_QDOC)
#include <experimental/source_location>
#define QT_PROPERTY_COLLECT_BINDING_LOCATION
#define QT_PROPERTY_DEFAULT_BINDING_LOCATION QPropertyBindingSourceLocation(std::source_location::current())
#elif __has_include(<experimental/source_location>) && __cplusplus >= 201703L && !defined(Q_CLANG_QDOC)
#include <experimental/source_location>
#define QT_PROPERTY_COLLECT_BINDING_LOCATION
#define QT_PROPERTY_DEFAULT_BINDING_LOCATION QPropertyBindingSourceLocation(std::experimental::source_location::current())
#else
#define QT_PROPERTY_DEFAULT_BINDING_LOCATION QPropertyBindingSourceLocation()
#endif

QT_BEGIN_NAMESPACE

template <typename T>
class QPropertyData : public QUntypedPropertyData
{
protected:
    mutable T val = T();
private:
    class DisableRValueRefs {};
protected:
    static constexpr bool UseReferences = !(std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_pointer_v<T>);
public:
    using value_type = T;
    using parameter_type = std::conditional_t<UseReferences, const T &, T>;
    using rvalue_ref = typename std::conditional_t<UseReferences, T &&, DisableRValueRefs>;
    using arrow_operator_result = std::conditional_t<!UseReferences, void,
                                        std::conditional_t<QTypeTraits::is_dereferencable_v<T>, parameter_type, const T *>>;

    QPropertyData() = default;
    QPropertyData(parameter_type t) : val(t) {}
    QPropertyData(rvalue_ref t) : val(std::move(t)) {}
    ~QPropertyData() = default;

    parameter_type valueBypassingBindings() const { return val; }
    void setValueBypassingBindings(parameter_type v) { val = v; }
    void setValueBypassingBindings(rvalue_ref v) { val = std::move(v); }
};

struct Q_CORE_EXPORT QPropertyBindingSourceLocation
{
    const char *fileName = nullptr;
    const char *functionName = nullptr;
    quint32 line = 0;
    quint32 column = 0;
    QPropertyBindingSourceLocation() = default;
#ifdef QT_PROPERTY_COLLECT_BINDING_LOCATION
    QPropertyBindingSourceLocation(const std::experimental::source_location &cppLocation)
    {
        fileName = cppLocation.file_name();
        functionName = cppLocation.function_name();
        line = cppLocation.line();
        column = cppLocation.column();
    }
#endif
};

template <typename Functor> class QPropertyChangeHandler;
class QPropertyBindingErrorPrivate;

class Q_CORE_EXPORT QPropertyBindingError
{
public:
    enum Type {
        NoError,
        BindingLoop,
        EvaluationError,
        UnknownError
    };

    QPropertyBindingError();
    QPropertyBindingError(Type type, const QString &description = QString());

    QPropertyBindingError(const QPropertyBindingError &other);
    QPropertyBindingError &operator=(const QPropertyBindingError &other);
    QPropertyBindingError(QPropertyBindingError &&other);
    QPropertyBindingError &operator=(QPropertyBindingError &&other);
    ~QPropertyBindingError();

    bool hasError() const { return d.get() != nullptr; }
    Type type() const;
    QString description() const;

private:
    QSharedDataPointer<QPropertyBindingErrorPrivate> d;
};

class Q_CORE_EXPORT QUntypedPropertyBinding
{
public:
    // writes binding result into dataPtr
    using BindingEvaluationFunction = QtPrivate::QPropertyBindingFunction;

    QUntypedPropertyBinding();
    QUntypedPropertyBinding(QMetaType metaType, BindingEvaluationFunction function, const QPropertyBindingSourceLocation &location);
    QUntypedPropertyBinding(QUntypedPropertyBinding &&other);
    QUntypedPropertyBinding(const QUntypedPropertyBinding &other);
    QUntypedPropertyBinding &operator=(const QUntypedPropertyBinding &other);
    QUntypedPropertyBinding &operator=(QUntypedPropertyBinding &&other);
    ~QUntypedPropertyBinding();

    bool isNull() const;

    QPropertyBindingError error() const;

    QMetaType valueMetaType() const;

    explicit QUntypedPropertyBinding(QPropertyBindingPrivate *priv);
private:
    friend class QtPrivate::QPropertyBindingData;
    friend class QPropertyBindingPrivate;
    template <typename> friend class QPropertyBinding;
    QPropertyBindingPrivatePtr d;
};

template <typename PropertyType>
class QPropertyBinding : public QUntypedPropertyBinding
{
    template <typename Functor>
    struct BindingAdaptor
    {
        Functor impl;
        bool operator()(QMetaType /*metaType*/, QUntypedPropertyData *dataPtr)
        {
            QPropertyData<PropertyType> *propertyPtr = static_cast<QPropertyData<PropertyType> *>(dataPtr);
            PropertyType newValue = impl();
            if constexpr (QTypeTraits::has_operator_equal_v<PropertyType>) {
                if (newValue == propertyPtr->valueBypassingBindings())
                    return false;
            }
            propertyPtr->setValueBypassingBindings(std::move(newValue));
            return true;
        }
    };

public:
    QPropertyBinding() = default;

    template<typename Functor>
    QPropertyBinding(Functor &&f, const QPropertyBindingSourceLocation &location)
        : QUntypedPropertyBinding(QMetaType::fromType<PropertyType>(), BindingAdaptor<Functor>{std::forward<Functor>(f)}, location)
    {}

    template<typename Property, typename = std::void_t<decltype(&Property::bindingData)>>
    QPropertyBinding(const Property &property)
        : QUntypedPropertyBinding(property.bindingData().binding())
    {}

    // Internal
    explicit QPropertyBinding(const QUntypedPropertyBinding &binding)
        : QUntypedPropertyBinding(binding)
    {}
};

namespace Qt {
    template <typename Functor>
    auto makePropertyBinding(Functor &&f, const QPropertyBindingSourceLocation &location = QT_PROPERTY_DEFAULT_BINDING_LOCATION,
                             std::enable_if_t<std::is_invocable_v<Functor>> * = 0)
    {
        return QPropertyBinding<std::invoke_result_t<Functor>>(std::forward<Functor>(f), location);
    }
}

struct QPropertyObserverPrivate;
struct QPropertyObserverPointer;

class Q_CORE_EXPORT QPropertyObserver
{
public:
    // Internal
    enum ObserverTag {
        ObserverNotifiesBinding,
        ObserverNotifiesChangeHandler,
        ObserverNotifiesAlias,
    };

    QPropertyObserver() = default;
    QPropertyObserver(QPropertyObserver &&other);
    QPropertyObserver &operator=(QPropertyObserver &&other);
    ~QPropertyObserver();

    template<typename Property, typename = std::enable_if<std::is_same_v<decltype(std::declval<Property>().bindingData()), QtPrivate::QPropertyBindingData &>>>
    void setSource(const Property &property)
    { setSource(property.bindingData()); }
    void setSource(const QtPrivate::QPropertyBindingData &property);

protected:
    using ChangeHandler = void (*)(QPropertyObserver*, QUntypedPropertyData *);
    QPropertyObserver(ChangeHandler changeHandler);
    QPropertyObserver(QUntypedPropertyData *aliasedPropertyPtr);

    QUntypedPropertyData *aliasedProperty() const
    {
        return aliasedPropertyData;
    }

private:

    QTaggedPointer<QPropertyObserver, ObserverTag> next;
    // prev is a pointer to the "next" element within the previous node, or to the "firstObserverPtr" if it is the
    // first node.
    QtPrivate::QTagPreservingPointerToPointer<QPropertyObserver, ObserverTag> prev;

    union {
        QPropertyBindingPrivate *bindingToMarkDirty = nullptr;
        ChangeHandler changeHandler;
        QUntypedPropertyData *aliasedPropertyData;
    };

    QPropertyObserver(const QPropertyObserver &) = delete;
    QPropertyObserver &operator=(const QPropertyObserver &) = delete;

    friend struct QPropertyObserverPointer;
    friend struct QPropertyBindingDataPointer;
    friend class QPropertyBindingPrivate;
};

template <typename Functor>
class QPropertyChangeHandler : public QPropertyObserver
{
    Functor m_handler;
public:
    QPropertyChangeHandler(Functor handler)
        : QPropertyObserver([](QPropertyObserver *self, QUntypedPropertyData *) {
              auto This = static_cast<QPropertyChangeHandler<Functor>*>(self);
              This->m_handler();
          })
        , m_handler(handler)
    {
    }

    template<typename Property, typename = std::void_t<decltype(&Property::bindingData)>>
    QPropertyChangeHandler(const Property &property, Functor handler)
        : QPropertyObserver([](QPropertyObserver *self, QUntypedPropertyData *) {
              auto This = static_cast<QPropertyChangeHandler<Functor>*>(self);
              This->m_handler();
          })
        , m_handler(handler)
    {
        setSource(property);
    }
};

template <typename T>
class QProperty : public QPropertyData<T>
{
    QtPrivate::QPropertyBindingData d;
    bool is_equal(const T &v)
    {
        if constexpr (QTypeTraits::has_operator_equal_v<T>) {
            if (v == this->val)
                return true;
        }
        return false;
    }

public:
    using value_type = typename QPropertyData<T>::value_type;
    using parameter_type = typename QPropertyData<T>::parameter_type;
    using rvalue_ref = typename QPropertyData<T>::rvalue_ref;
    using arrow_operator_result = typename QPropertyData<T>::arrow_operator_result;

    QProperty() = default;
    explicit QProperty(parameter_type initialValue) : QPropertyData<T>(initialValue) {}
    explicit QProperty(rvalue_ref initialValue) : QPropertyData<T>(std::move(initialValue)) {}
    QProperty(QProperty &&other) : QPropertyData<T>(std::move(other.val)), d(std::move(other.d), this) { notify(); }
    explicit QProperty(const QPropertyBinding<T> &binding)
        : QProperty()
    { setBinding(binding); }
#ifndef Q_CLANG_QDOC
    template <typename Functor>
    explicit QProperty(Functor &&f, const QPropertyBindingSourceLocation &location = QT_PROPERTY_DEFAULT_BINDING_LOCATION,
                       typename std::enable_if_t<std::is_invocable_r_v<T, Functor&>> * = 0)
        : QProperty(QPropertyBinding<T>(std::forward<Functor>(f), location))
    {}
#else
    template <typename Functor>
    explicit QProperty(Functor &&f);
#endif
    QProperty &operator=(QProperty &&other) { this->val = std::move(other.val); d.moveAssign(std::move(other.d), this); notify(); return *this; }
    ~QProperty() = default;

    parameter_type value() const
    {
        if (d.hasBinding())
            d.evaluateIfDirty(this);
        d.registerWithCurrentlyEvaluatingBinding();
        return this->val;
    }

    arrow_operator_result operator->() const
    {
        if constexpr (QTypeTraits::is_dereferencable_v<T>)
            return value();
        else if constexpr (QPropertyData<T>::UseReferences)
            return &value();
        else
            return;
    }

    parameter_type operator*() const
    {
        return value();
    }

    operator parameter_type() const
    {
        return value();
    }

    void setValue(rvalue_ref newValue)
    {
        d.removeBinding();
        if (is_equal(newValue))
            return;
        this->val = std::move(newValue);
        notify();
    }

    void setValue(parameter_type newValue)
    {
        d.removeBinding();
        if (is_equal(newValue))
            return;
        this->val = newValue;
        notify();
    }

    QProperty<T> &operator=(rvalue_ref newValue)
    {
        setValue(std::move(newValue));
        return *this;
    }

    QProperty<T> &operator=(parameter_type newValue)
    {
        setValue(newValue);
        return *this;
    }

    QPropertyBinding<T> setBinding(const QPropertyBinding<T> &newBinding)
    {
        QPropertyBinding<T> oldBinding(d.setBinding(newBinding, this));
        notify();
        return oldBinding;
    }

    bool setBinding(const QUntypedPropertyBinding &newBinding)
    {
        if (!newBinding.isNull() && newBinding.valueMetaType().id() != qMetaTypeId<T>())
            return false;
        setBinding(static_cast<const QPropertyBinding<T> &>(newBinding));
        return true;
    }

#ifndef Q_CLANG_QDOC
    template <typename Functor>
    QPropertyBinding<T> setBinding(Functor &&f,
                                   const QPropertyBindingSourceLocation &location = QT_PROPERTY_DEFAULT_BINDING_LOCATION,
                                   std::enable_if_t<std::is_invocable_v<Functor>> * = nullptr)
    {
        return setBinding(Qt::makePropertyBinding(std::forward<Functor>(f), location));
    }
#else
    template <typename Functor>
    QPropertyBinding<T> setBinding(Functor f);
#endif

    bool hasBinding() const { return d.hasBinding(); }

    QPropertyBinding<T> binding() const
    {
        return QPropertyBinding<T>(*this);
    }

    QPropertyBinding<T> takeBinding()
    {
        return QPropertyBinding<T>(d.setBinding(QUntypedPropertyBinding(), this));
    }

    template<typename Functor>
    QPropertyChangeHandler<Functor> onValueChanged(Functor f)
    {
#if defined(__cpp_lib_is_invocable) && (__cpp_lib_is_invocable >= 201703L)
        static_assert(std::is_invocable_v<Functor>, "Functor callback must be callable without any parameters");
#endif
        return QPropertyChangeHandler<Functor>(*this, f);
    }

    template<typename Functor>
    QPropertyChangeHandler<Functor> subscribe(Functor f)
    {
#if defined(__cpp_lib_is_invocable) && (__cpp_lib_is_invocable >= 201703L)
        static_assert(std::is_invocable_v<Functor>, "Functor callback must be callable without any parameters");
#endif
        f();
        return onValueChanged(f);
    }

    const QtPrivate::QPropertyBindingData &bindingData() const { return d; }
private:
    void notify()
    {
        d.notifyObservers(this);
    }

    Q_DISABLE_COPY(QProperty)
};

namespace Qt {
    template <typename PropertyType>
    QPropertyBinding<PropertyType> makePropertyBinding(const QProperty<PropertyType> &otherProperty,
                                                       const QPropertyBindingSourceLocation &location =
                                                       QT_PROPERTY_DEFAULT_BINDING_LOCATION)
    {
        return Qt::makePropertyBinding([&otherProperty]() -> PropertyType { return otherProperty; }, location);
    }
}

template<typename T>
class QPropertyAlias : public QPropertyObserver
{
    Q_DISABLE_COPY_MOVE(QPropertyAlias)
    QProperty<T> *aliasedProperty() const
    {
        return static_cast<QProperty<T> *>(QPropertyObserver::aliasedProperty());
    }

public:
    QPropertyAlias(QProperty<T> *property)
        : QPropertyObserver(property)
    {
        if (property)
            setSource(*property);
    }

    QPropertyAlias(QPropertyAlias<T> *alias)
        : QPropertyAlias(alias->aliasedProperty())
    {}

    T value() const
    {
        if (auto *p = aliasedProperty())
            return p->value();
        return T();
    }

    operator T() const { return value(); }

    void setValue(T &&newValue)
    {
        if (auto *p = aliasedProperty())
            p->setValue(std::move(newValue));
    }

    void setValue(const T &newValue)
    {
        if (auto *p = aliasedProperty())
            p->setValue(newValue);
    }

    QPropertyAlias<T> &operator=(T &&newValue)
    {
        if (auto *p = aliasedProperty())
            *p = std::move(newValue);
        return *this;
    }

    QPropertyAlias<T> &operator=(const T &newValue)
    {
        if (auto *p = aliasedProperty())
            *p = newValue;
        return *this;
    }

    QPropertyBinding<T> setBinding(const QPropertyBinding<T> &newBinding)
    {
        if (auto *p = aliasedProperty())
            return p->setBinding(newBinding);
        return QPropertyBinding<T>();
    }

    QPropertyBinding<T> setBinding(QPropertyBinding<T> &&newBinding)
    {
        if (auto *p = aliasedProperty())
            return p->setBinding(std::move(newBinding));
        return QPropertyBinding<T>();
    }

    bool setBinding(const QUntypedPropertyBinding &newBinding)
    {
        if (auto *p = aliasedProperty())
            return p->setBinding(newBinding);
        return false;
    }

#ifndef Q_CLANG_QDOC
    template <typename Functor>
    QPropertyBinding<T> setBinding(Functor &&f,
                                   const QPropertyBindingSourceLocation &location = QT_PROPERTY_DEFAULT_BINDING_LOCATION,
                                   std::enable_if_t<std::is_invocable_v<Functor>> * = nullptr)
    {
        return setBinding(Qt::makePropertyBinding(std::forward<Functor>(f), location));
    }
#else
    template <typename Functor>
    QPropertyBinding<T> setBinding(Functor f);
#endif

    bool hasBinding() const
    {
        if (auto *p = aliasedProperty())
            return p->hasBinding();
        return false;
    }

    QPropertyBinding<T> binding() const
    {
        if (auto *p = aliasedProperty())
            return p->binding();
        return QPropertyBinding<T>();
    }

    QPropertyBinding<T> takeBinding()
    {
        if (auto *p = aliasedProperty())
            return p->takeBinding();
        return QPropertyBinding<T>();
    }

    template<typename Functor>
    QPropertyChangeHandler<Functor> onValueChanged(Functor f)
    {
        if (auto *p = aliasedProperty())
            return p->onValueChanged(f);
        return QPropertyChangeHandler<Functor>(f);
    }

    template<typename Functor>
    QPropertyChangeHandler<Functor> subscribe(Functor f)
    {
        if (auto *p = aliasedProperty())
            return p->subscribe(f);
        return QPropertyChangeHandler<Functor>(f);
    }

    bool isValid() const
    {
        return aliasedProperty() != nullptr;
    }
};

namespace QtPrivate
{

struct QBindableInterface
{
    using BindingGetter = QUntypedPropertyBinding (*)(const QUntypedPropertyData *d);
    using BindingSetter = QUntypedPropertyBinding (*)(QUntypedPropertyData *d, const QUntypedPropertyBinding &binding);
    using MakeBinding = QUntypedPropertyBinding (*)(const QUntypedPropertyData *d, const QPropertyBindingSourceLocation &location);
    using SetObserver = void (*)(const QUntypedPropertyData *d, QPropertyObserver *observer);
    BindingGetter getBinding;
    BindingSetter setBinding;
    MakeBinding makeBinding;
    SetObserver setObserver;
    QMetaType metaType;
};

template<typename Property, typename = void>
class QBindableInterfaceForProperty
{
    using T = typename Property::value_type;
public:
    static constexpr QBindableInterface interface = {
        nullptr,
        nullptr,
        [](const QUntypedPropertyData *d, const QPropertyBindingSourceLocation &location) -> QUntypedPropertyBinding
        { return Qt::makePropertyBinding([d]() -> T { return static_cast<const Property *>(d)->value(); }, location); },
        [](const QUntypedPropertyData *d, QPropertyObserver *observer) -> void
        { observer->setSource(static_cast<const Property *>(d)->bindingData()); },
        QMetaType::fromType<T>()
    };
};

template<typename Property>
class QBindableInterfaceForProperty<Property, std::void_t<decltype(std::declval<Property>().binding())>>
{
    using T = typename Property::value_type;
public:
    static constexpr QBindableInterface interface = {
        [](const QUntypedPropertyData *d) -> QUntypedPropertyBinding
        { return static_cast<const Property *>(d)->binding(); },
        [](QUntypedPropertyData *d, const QUntypedPropertyBinding &binding) -> QUntypedPropertyBinding
        { return static_cast<Property *>(d)->setBinding(static_cast<const QPropertyBinding<T> &>(binding)); },
        [](const QUntypedPropertyData *d, const QPropertyBindingSourceLocation &location) -> QUntypedPropertyBinding
        { return Qt::makePropertyBinding([d]() -> T { return static_cast<const Property *>(d)->value(); }, location); },
        [](const QUntypedPropertyData *d, QPropertyObserver *observer) -> void
        { observer->setSource(static_cast<const Property *>(d)->bindingData()); },
        QMetaType::fromType<T>()
    };
};

}

class QUntypedBindable
{
protected:
    QUntypedPropertyData *data = nullptr;
    const QtPrivate::QBindableInterface *iface = nullptr;

public:
    constexpr QUntypedBindable() = default;
    template<typename Property>
    QUntypedBindable(Property *p)
        : data(const_cast<std::remove_cv_t<Property> *>(p)),
          iface(&QtPrivate::QBindableInterfaceForProperty<Property>::interface)
    { Q_ASSERT(data && iface); }

    bool isValid() const { return data != nullptr; }
    bool isBindable() const { return iface && iface->getBinding; }

    QUntypedPropertyBinding makeBinding(const QPropertyBindingSourceLocation &location = QT_PROPERTY_DEFAULT_BINDING_LOCATION)
    {
        return iface->makeBinding(data, location);
    }
    void observe(QPropertyObserver *observer)
    {
        iface->setObserver(data, observer);
    }

    template<typename Functor>
    QPropertyChangeHandler<Functor> onValueChanged(Functor f)
    {
        QPropertyChangeHandler<Functor> handler(f);
        observe(&handler);
        return handler;
    }

    template<typename Functor>
    QPropertyChangeHandler<Functor> subscribe(Functor f)
    {
        f();
        return onValueChanged(f);
    }

    QUntypedPropertyBinding binding() const
    {
        if (!iface->getBinding)
            return QUntypedPropertyBinding();
        return iface->getBinding(data);
    }
    bool setBinding(const QUntypedPropertyBinding &binding)
    {
        if (!iface->setBinding)
            return false;
        if (!binding.isNull() && binding.valueMetaType() != iface->metaType)
            return false;
        iface->setBinding(data, binding);
        return true;
    }
    bool hasBinding() const
    {
        return !binding().isNull();
    }

};

template<typename T>
class QBindable : public QUntypedBindable
{
public:
    using QUntypedBindable::QUntypedBindable;
    explicit QBindable(const QUntypedBindable &b) : QUntypedBindable(b) {}

    QPropertyBinding<T> makeBinding(const QPropertyBindingSourceLocation &location = QT_PROPERTY_DEFAULT_BINDING_LOCATION)
    {
        return static_cast<QPropertyBinding<T> &&>(iface->makeBinding(data, location));
    }
    QPropertyBinding<T> binding() const
    {
        return static_cast<QPropertyBinding<T> &&>(QUntypedBindable::binding());
    }
    using QUntypedBindable::setBinding;
    QPropertyBinding<T> setBinding(const QPropertyBinding<T> &binding)
    {
        return static_cast<QPropertyBinding<T> &&>(iface->setBinding(data, binding));
    }
#ifndef Q_CLANG_QDOC
    template <typename Functor>
    QPropertyBinding<T> setBinding(Functor &&f,
                                   const QPropertyBindingSourceLocation &location = QT_PROPERTY_DEFAULT_BINDING_LOCATION,
                                   std::enable_if_t<std::is_invocable_v<Functor>> * = nullptr)
    {
        return setBinding(Qt::makePropertyBinding(std::forward<Functor>(f), location));
    }
#else
    template <typename Functor>
    QPropertyBinding<T> setBinding(Functor f);
#endif
};

struct QBindingStatus;

struct QBindingStorageData;
class Q_CORE_EXPORT QBindingStorage
{
    mutable QBindingStorageData *d = nullptr;
    QBindingStatus *bindingStatus = nullptr;

    template<typename Class, typename T, auto Offset, auto Setter>
    friend class QObjectCompatProperty;
public:
    QBindingStorage();
    ~QBindingStorage();

    void maybeUpdateBindingAndRegister(const QUntypedPropertyData *data) const;
    QtPrivate::QPropertyBindingData *bindingData(const QUntypedPropertyData *data) const;
    QtPrivate::QPropertyBindingData *bindingData(QUntypedPropertyData *data, bool create);
};


template<typename Class, typename T, auto Offset, auto Signal = nullptr>
class QObjectBindableProperty : public QPropertyData<T>
{
    using ThisType = QObjectBindableProperty<Class, T, Offset, Signal>;
    static bool constexpr HasSignal = !std::is_same_v<decltype(Signal), std::nullptr_t>;
    static constexpr size_t offset = QtPrivate::detail::getOffset(Offset);
    Class *owner()
    {
        char *that = reinterpret_cast<char *>(this);
        return reinterpret_cast<Class *>(that - offset);
    }
    const Class *owner() const
    {
        char *that = const_cast<char *>(reinterpret_cast<const char *>(this));
        return reinterpret_cast<Class *>(that - offset);
    }
    static void signalCallBack(QUntypedPropertyData *o)
    {
        QObjectBindableProperty *that = static_cast<QObjectBindableProperty *>(o);
        if constexpr (HasSignal)
            (that->owner()->*Signal)();
    }
public:
    using value_type = typename QPropertyData<T>::value_type;
    using parameter_type = typename QPropertyData<T>::parameter_type;
    using rvalue_ref = typename QPropertyData<T>::rvalue_ref;
    using arrow_operator_result = typename QPropertyData<T>::arrow_operator_result;

    QObjectBindableProperty() = default;
    explicit QObjectBindableProperty(const T &initialValue) : QPropertyData<T>(initialValue) {}
    explicit QObjectBindableProperty(T &&initialValue) : QPropertyData<T>(std::move(initialValue)) {}
    explicit QObjectBindableProperty(const QPropertyBinding<T> &binding)
        : QObjectBindableProperty()
    { setBinding(binding); }
#ifndef Q_CLANG_QDOC
    template <typename Functor>
    explicit QObjectBindableProperty(Functor &&f, const QPropertyBindingSourceLocation &location = QT_PROPERTY_DEFAULT_BINDING_LOCATION,
                       typename std::enable_if_t<std::is_invocable_r_v<T, Functor&>> * = 0)
        : QObjectBindableProperty(QPropertyBinding<T>(std::forward<Functor>(f), location))
    {}
#else
    template <typename Functor>
    explicit QProperty(Functor &&f);
#endif

    parameter_type value() const {
        qGetBindingStorage(owner())->maybeUpdateBindingAndRegister(this);
        return this->val;
    }

    arrow_operator_result operator->() const
    {
        if constexpr (QTypeTraits::is_dereferencable_v<T>)
            return value();
        else if constexpr (QPropertyData<T>::UseReferences)
            return &value();
        else
            return;
    }

    parameter_type operator*() const
    {
        return value();
    }

    operator parameter_type() const
    {
        return value();
    }

    void setValue(parameter_type t) {
        auto *bd = qGetBindingStorage(owner())->bindingData(this);
        if (bd)
            bd->removeBinding();
        if (this->val == t)
            return;
        this->val = t;
        notify(bd);
    }

    void setValue(rvalue_ref t) {
        auto *bd = qGetBindingStorage(owner())->bindingData(this);
        if (bd)
            bd->removeBinding();
        if (this->val == t)
            return;
        this->val = std::move(t);
        notify(bd);
    }

    QObjectBindableProperty &operator=(rvalue_ref newValue)
    {
        setValue(std::move(newValue));
        return *this;
    }

    QObjectBindableProperty &operator=(parameter_type newValue)
    {
        setValue(newValue);
        return *this;
    }

    QPropertyBinding<T> setBinding(const QPropertyBinding<T> &newBinding)
    {
        QtPrivate::QPropertyBindingData *bd = qGetBindingStorage(owner())->bindingData(this, true);
        QUntypedPropertyBinding oldBinding(bd->setBinding(newBinding, this, HasSignal ? &signalCallBack : nullptr));
        notify(bd);
        return static_cast<QPropertyBinding<T> &>(oldBinding);
    }

    bool setBinding(const QUntypedPropertyBinding &newBinding)
    {
        if (!newBinding.isNull() && newBinding.valueMetaType().id() != qMetaTypeId<T>())
            return false;
        setBinding(static_cast<const QPropertyBinding<T> &>(newBinding));
        return true;
    }

#ifndef Q_CLANG_QDOC
    template <typename Functor>
    QPropertyBinding<T> setBinding(Functor &&f,
                                   const QPropertyBindingSourceLocation &location = QT_PROPERTY_DEFAULT_BINDING_LOCATION,
                                   std::enable_if_t<std::is_invocable_v<Functor>> * = nullptr)
    {
        return setBinding(Qt::makePropertyBinding(std::forward<Functor>(f), location));
    }
#else
    template <typename Functor>
    QPropertyBinding<T> setBinding(Functor f);
#endif

    bool hasBinding() const {
        auto *bd = qGetBindingStorage(owner())->bindingData(this);
        return bd && bd->binding() != nullptr;
    }

    QPropertyBinding<T> binding() const
    {
        auto *bd = qGetBindingStorage(owner())->bindingData(this);
        return static_cast<QPropertyBinding<T> &&>(QUntypedPropertyBinding(bd ? bd->binding() : nullptr));
    }

    QPropertyBinding<T> takeBinding()
    {
        return setBinding(QPropertyBinding<T>());
    }

    template<typename Functor>
    QPropertyChangeHandler<Functor> onValueChanged(Functor f)
    {
#if defined(__cpp_lib_is_invocable) && (__cpp_lib_is_invocable >= 201703L)
        static_assert(std::is_invocable_v<Functor>, "Functor callback must be callable without any parameters");
#endif
        return QPropertyChangeHandler<Functor>(*this, f);
    }

    template<typename Functor>
    QPropertyChangeHandler<Functor> subscribe(Functor f)
    {
#if defined(__cpp_lib_is_invocable) && (__cpp_lib_is_invocable >= 201703L)
        static_assert(std::is_invocable_v<Functor>, "Functor callback must be callable without any parameters");
#endif
        f();
        return onValueChanged(f);
    }

    const QtPrivate::QPropertyBindingData &bindingData() const
    {
        auto *storage = const_cast<QBindingStorage *>(qGetBindingStorage(owner()));
        return *storage->bindingData(const_cast<ThisType *>(this), true);
    }
private:
    void notify(const QtPrivate::QPropertyBindingData *binding)
    {
        if (binding)
            binding->notifyObservers(this);
        if constexpr (HasSignal)
            (owner()->*Signal)();
    }
};

#define Q_OBJECT_BINDABLE_PROPERTY(Class, Type, name,  ...) \
    static constexpr size_t _qt_property_##name##_offset() { \
        QT_WARNING_PUSH QT_WARNING_DISABLE_INVALID_OFFSETOF \
        return offsetof(Class, name); \
        QT_WARNING_POP \
    } \
    QObjectBindableProperty<Class, Type, Class::_qt_property_##name##_offset, __VA_ARGS__> name;

template<typename Class, typename T, auto Offset, auto Getter>
class QObjectComputedProperty : public QUntypedPropertyData
{
    static constexpr size_t offset = QtPrivate::detail::getOffset(Offset);
    Class *owner()
    {
        char *that = reinterpret_cast<char *>(this);
        return reinterpret_cast<Class *>(that - offset);
    }
    const Class *owner() const
    {
        char *that = const_cast<char *>(reinterpret_cast<const char *>(this));
        return reinterpret_cast<Class *>(that - offset);
    }
public:
    using value_type = T;
    using parameter_type = T;

    QObjectComputedProperty() = default;

    parameter_type value() const {
        qGetBindingStorage(owner())->maybeUpdateBindingAndRegister(this);
        return (owner()->*Getter)();
    }

    std::conditional_t<QTypeTraits::is_dereferencable_v<T>, parameter_type, void>
    operator->() const
    {
        if constexpr (QTypeTraits::is_dereferencable_v<T>)
            return value();
        else
            return;
    }

    parameter_type operator*() const
    {
        return value();
    }

    operator parameter_type() const
    {
        return value();
    }

    constexpr bool hasBinding() const { return false; }

    template<typename Functor>
    QPropertyChangeHandler<Functor> onValueChanged(Functor f)
    {
#if defined(__cpp_lib_is_invocable) && (__cpp_lib_is_invocable >= 201703L)
        static_assert(std::is_invocable_v<Functor>, "Functor callback must be callable without any parameters");
#endif
        return QPropertyChangeHandler<Functor>(*this, f);
    }

    template<typename Functor>
    QPropertyChangeHandler<Functor> subscribe(Functor f)
    {
#if defined(__cpp_lib_is_invocable) && (__cpp_lib_is_invocable >= 201703L)
        static_assert(std::is_invocable_v<Functor>, "Functor callback must be callable without any parameters");
#endif
        f();
        return onValueChanged(f);
    }

    QtPrivate::QPropertyBindingData &bindingData() const
    {
        auto *storage = const_cast<QBindingStorage *>(qGetBindingStorage(owner()));
        return *storage->bindingData(const_cast<QObjectComputedProperty *>(this), true);
    }
private:
};

#define Q_OBJECT_COMPUTED_PROPERTY(Class, Type, name,  ...) \
    static constexpr size_t _qt_property_##name##_offset() { \
        QT_WARNING_PUSH QT_WARNING_DISABLE_INVALID_OFFSETOF \
        return offsetof(Class, name); \
        QT_WARNING_POP \
    } \
    QObjectComputedProperty<Class, Type, Class::_qt_property_##name##_offset, __VA_ARGS__> name;

QT_END_NAMESPACE

#endif // QPROPERTY_H
