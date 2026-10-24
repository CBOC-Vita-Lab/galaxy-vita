// Non-standard helpers and pre-C++17 functional adaptors that the game code
// expects from Metrowerks' MSL C++ library.  libc++ in C++20 mode no longer
// declares the adaptors, so defining them here does not collide with it.
#pragma once

#include <algorithm>
#include <functional>
#include <iterator>

// libstdc++ retains the old adaptors, but its binder2nd lacks MSL's explicit
// bound-value type. Keep the game's spelling without redefining that class.
#ifdef __GLIBCXX__
#define binder2nd port_msl_binder2nd
#define bind2nd port_msl_bind2nd
#endif

namespace std {

    // ---- MSL extensions -------------------------------------------------
    // Like for_each, but hands the callback a pointer to each element.
    template < class T, class Function >
    inline Function for_each_array(T* pFirst, T* pLast, Function f) {
        for (; pFirst != pLast; pFirst++) {
            f(pFirst);
        }
        return f;
    }

    template < class T, class UnaryPredicate >
    inline T* find_if_array(T* pFirst, T* pLast, UnaryPredicate p) {
        for (; pFirst != pLast && !p(pFirst); pFirst++) {
        }
        return pFirst;
    }

    template < class InputIt, class UnaryPredicate >
    inline InputIt rfind_if(InputIt first, InputIt last, UnaryPredicate p) {
        for (; first != last && !p(*first); first--) {
        }
        return first;
    }

#ifndef __GLIBCXX__
    // ---- C++98 adaptor base classes ---------------------------------------
    template < class Arg, class Result >
    struct unary_function {
        typedef Arg argument_type;
        typedef Result result_type;
    };

    template < class Arg1, class Arg2, class Result >
    struct binary_function {
        typedef Arg1 first_argument_type;
        typedef Arg2 second_argument_type;
        typedef Result result_type;
    };

    // ---- binders ------------------------------------------------------------
    template < class Func >
    class binder1st : public unary_function< typename Func::second_argument_type, typename Func::result_type > {
    public:
        binder1st(const Func& f, const typename Func::first_argument_type& v) : op(f), value(v) {}
        typename Func::result_type operator()(const typename Func::second_argument_type& x) const { return op(value, x); }
        typename Func::result_type operator()(typename Func::second_argument_type& x) const { return op(value, x); }

    protected:
        Func op;
        typename Func::first_argument_type value;
    };

    template < class Func, class Type >
    inline binder1st< Func > bind1st(const Func& f, const Type& v) {
        return binder1st< Func >(f, typename Func::first_argument_type(v));
    }
#endif

    template < class Func, class Type = typename Func::second_argument_type >
    class binder2nd : public unary_function< typename Func::first_argument_type, typename Func::result_type > {
    public:
        binder2nd(const Func& f, const Type& v) : op(f), value(v) {}
        typename Func::result_type operator()(const typename Func::first_argument_type& x) const { return op(x, value); }
        typename Func::result_type operator()(typename Func::first_argument_type& x) const { return op(x, value); }

    protected:
        Func op;
        Type value;
    };

    template < class Func, class Type >
    inline binder2nd< Func, typename Func::second_argument_type > bind2nd(const Func& f, const Type& v) {
        return binder2nd< Func, typename Func::second_argument_type >(f, typename Func::second_argument_type(v));
    }

#ifndef __GLIBCXX__
    // ---- member function adaptors ---------------------------------------------
    template < class Return, class Type >
    class mem_fun_t : public unary_function< Type*, Return > {
    public:
        explicit mem_fun_t(Return (Type::*p)()) : f(p) {}
        Return operator()(Type* p) const { return (p->*f)(); }

    private:
        Return (Type::*f)();
    };

    template < class Return, class Type >
    class mem_fun_ref_t : public unary_function< Type, Return > {
    public:
        explicit mem_fun_ref_t(Return (Type::*p)()) : f(p) {}
        Return operator()(Type& r) const { return (r.*f)(); }

    private:
        Return (Type::*f)();
    };

    template < class Return, class Type >
    class const_mem_fun_t : public unary_function< const Type*, Return > {
    public:
        explicit const_mem_fun_t(Return (Type::*p)() const) : f(p) {}
        Return operator()(const Type* p) const { return (p->*f)(); }

    private:
        Return (Type::*f)() const;
    };

    template < class Result, class Type, class Arg >
    class mem_fun1_t : public binary_function< Type*, Arg, Result > {
    public:
        explicit mem_fun1_t(Result (Type::*p)(Arg)) : f(p) {}
        Result operator()(Type* p, Arg x) const { return (p->*f)(x); }

    private:
        Result (Type::*f)(Arg);
    };

    template < class Result, class Type, class Arg >
    class mem_fun1_ref_t : public binary_function< Type, Arg, Result > {
    public:
        explicit mem_fun1_ref_t(Result (Type::*p)(Arg)) : f(p) {}
        Result operator()(Type& r, Arg x) const { return (r.*f)(x); }

    private:
        Result (Type::*f)(Arg);
    };

    template < class Result, class Type, class Arg >
    class const_mem_fun1_t : public binary_function< const Type*, Arg, Result > {
    public:
        explicit const_mem_fun1_t(Result (Type::*p)(Arg) const) : f(p) {}
        Result operator()(const Type* p, Arg x) const { return (p->*f)(x); }

    private:
        Result (Type::*f)(Arg) const;
    };

    template < class Result, class Type >
    inline mem_fun_t< Result, Type > mem_fun(Result (Type::*p)()) { return mem_fun_t< Result, Type >(p); }
    template < class Result, class Type >
    inline const_mem_fun_t< Result, Type > mem_fun(Result (Type::*p)() const) { return const_mem_fun_t< Result, Type >(p); }
    template < class Result, class Type, class Arg >
    inline mem_fun1_t< Result, Type, Arg > mem_fun(Result (Type::*p)(Arg)) { return mem_fun1_t< Result, Type, Arg >(p); }
    template < class Result, class Type, class Arg >
    inline const_mem_fun1_t< Result, Type, Arg > mem_fun(Result (Type::*p)(Arg) const) { return const_mem_fun1_t< Result, Type, Arg >(p); }

    template < class Result, class Type >
    inline mem_fun_ref_t< Result, Type > mem_fun_ref(Result (Type::*p)()) { return mem_fun_ref_t< Result, Type >(p); }
    template < class Result, class Type, class Arg >
    inline mem_fun1_ref_t< Result, Type, Arg > mem_fun_ref(Result (Type::*p)(Arg)) { return mem_fun1_ref_t< Result, Type, Arg >(p); }
#endif

    // MSL spelling of mem_fun.
    template < class Result, class Type >
    inline mem_fun_t< Result, Type > mem_func(Result (Type::*p)()) { return mem_fun_t< Result, Type >(p); }
    template < class Result, class Type >
    inline const_mem_fun_t< Result, Type > mem_func(Result (Type::*p)() const) { return const_mem_fun_t< Result, Type >(p); }
    template < class Result, class Type, class Arg >
    inline mem_fun1_t< Result, Type, Arg > mem_func(Result (Type::*p)(Arg)) { return mem_fun1_t< Result, Type, Arg >(p); }
    template < class Result, class Type, class Arg >
    inline const_mem_fun1_t< Result, Type, Arg > mem_func(Result (Type::*p)(Arg) const) { return const_mem_fun1_t< Result, Type, Arg >(p); }

#ifndef __GLIBCXX__
    // ---- negators / function pointers ------------------------------------------
    template < class Predicate >
    class unary_negate : public unary_function< typename Predicate::argument_type, bool > {
    public:
        explicit unary_negate(const Predicate& p) : pred(p) {}
        bool operator()(const typename Predicate::argument_type& x) const { return !pred(x); }

    private:
        Predicate pred;
    };

    template < class Predicate >
    inline unary_negate< Predicate > not1(const Predicate& p) {
        return unary_negate< Predicate >(p);
    }

    template < class Arg, class Result >
    class pointer_to_unary_function : public unary_function< Arg, Result > {
    public:
        explicit pointer_to_unary_function(Result (*p)(Arg)) : f(p) {}
        Result operator()(Arg x) const { return f(x); }

    private:
        Result (*f)(Arg);
    };

    template < class Arg, class Result >
    inline pointer_to_unary_function< Arg, Result > ptr_fun(Result (*p)(Arg)) {
        return pointer_to_unary_function< Arg, Result >(p);
    }

    template < class Arg1, class Arg2, class Result >
    class pointer_to_binary_function : public binary_function< Arg1, Arg2, Result > {
    public:
        explicit pointer_to_binary_function(Result (*p)(Arg1, Arg2)) : f(p) {}
        Result operator()(Arg1 x, Arg2 y) const { return f(x, y); }

    private:
        Result (*f)(Arg1, Arg2);
    };

    template < class Arg1, class Arg2, class Result >
    inline pointer_to_binary_function< Arg1, Arg2, Result > ptr_fun(Result (*p)(Arg1, Arg2)) {
        return pointer_to_binary_function< Arg1, Arg2, Result >(p);
    }
#endif

}  // namespace std
