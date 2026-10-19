/*
 * Compat.h -- the only place that knows about differences between OPENSTEP 4.2 and the modern SDK used on
 * the development Mac to syntax-check these sources (`make check-objc`, never linked or run).
 */
#ifndef COMPAT_H
#define COMPAT_H

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

/* [V] Characters delivered by -[NSEvent characters] for function keys: the Unicode private-use codes
 * NSEvent.h calls NSUpArrowFunctionKey etc.  Named differently here so they cannot collide with the system
 * header. */
#define KEYCH_UP        0xF700
#define KEYCH_DOWN      0xF701

#endif
