// util.h
//
// Copyright (C) 2001, Chris Laurel <claurel@shatters.net>
//
// Miscellaneous useful functions.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#ifndef _CELUTIL_UTIL_H_
#define _CELUTIL_UTIL_H_

#include <string>
#include <iostream>
#include <functional>
#include <libintl.h>

// gettext / libintl setup
#ifndef _ /* unless somebody already took care of this */
#define _(string) gettext (string)
#endif
#ifndef gettext_noop
#define gettext_noop(string) string
#endif

#ifdef _WIN32
// POSIX provides an extension to printf family to reorder their arguments,
// so GNU GetText provides own replacement for them on windows platform
#ifdef fprintf
#undef fprintf
#endif
#ifdef printf
#undef printf
#endif
#ifdef sprintf
#undef sprintf
#endif
#endif

extern int compareIgnoringCase(const std::string& s1, const std::string& s2);
extern int compareIgnoringCase(const std::string& s1, const std::string& s2, int n);
extern std::string LocaleFilename(const std::string & filename);

class CompareIgnoringCasePredicate : public std::binary_function<std::string, std::string, bool>
{
 public:
    bool operator()(const std::string&, const std::string&) const;
};

template <class T> struct printlineFunc : public std::unary_function<T, void>
{
    printlineFunc(std::ostream& o) : out(o) {};
    void operator() (T x) { out << x << '\n'; };
    std::ostream& out;
};

template <class T> struct deleteFunc : public std::unary_function<T, void>
{
    deleteFunc() {};
    void operator() (T x) { delete x; };
    int dummy;
};

#endif // _CELUTIL_UTIL_H_
