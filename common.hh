#ifndef COMMON_H
#define COMMON_H 1

#include <math.h>
#include <sys/time.h>

// Stolen from http://google-styleguide.googlecode.com/svn/trunk/cppguide.xml
// A macro to disallow the copy constructor and operator= functions
// This should be used in the private: declarations for a class
#define DISALLOW_COPY_AND_ASSIGN(TypeName)      \
    TypeName(const TypeName&);                  \
    void operator=(const TypeName&)

inline void advance_tv(struct timeval &tv, const double secs) {
    double ip, fp;
    fp = modf(secs, &ip);
    int usec = (fp * 1e6) + tv.tv_usec;
    int quot = usec / 1000000;
    int rem = usec % 1000000;
    tv.tv_sec = ip + tv.tv_sec + quot;
    tv.tv_usec = rem;
}

inline bool less_tv(const struct timeval &tv1, const struct timeval &tv2) {
    if (tv1.tv_sec == tv2.tv_sec) {
        return tv1.tv_usec < tv2.tv_usec;
    } else {
        return tv1.tv_sec < tv2.tv_sec;
    }
}

#endif /* COMMON_H */
