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

/* OPENSTEP 4.2's AppKit does not declare NSOnState / NSOffState ("undeclared": found on i386 when StepAmp was first
 * built there).  -setState: takes a plain int on that release, 1 for on and 0 for off; the modern SDK has them as
 * enum constants, so they are only defined for OPENSTEP. */
#ifdef OPENSTEP
#define NSOnState  1
#define NSOffState 0
#endif

/* OPENSTEP 4.2 has no NSDragOperation type: the drag-destination methods return an unsigned int there, and "copy" is 1. */
#ifdef OPENSTEP
typedef unsigned int KDragOp;
#else
typedef NSDragOperation KDragOp;
#endif

#endif
