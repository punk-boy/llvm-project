# REQUIRES: x86

# RUN: llvm-mc -filetype=obj -triple=x86_64 %s -o %t1.o
# RUN: echo '.globl foo; foo:' | llvm-mc -filetype=obj -triple=x86_64 - -o %t2.o
# RUN: rm -f %t2.a
# RUN: llvm-ar rcs %t2.a %t2.o
# RUN: ld.lld -shared %t2.o -o %t2.so

## A forward reference is accepted by a traditional Unix linker.
# RUN: ld.lld --fatal-warnings %t1.o %t2.a -o /dev/null
# RUN: ld.lld --fatal-warnings --warn-backrefs %t1.o %t2.a -o /dev/null
# RUN: ld.lld --fatal-warnings --warn-backrefs %t1.o --start-lib %t2.o --end-lib -o /dev/null

# RUN: echo 'INPUT("%t1.o" "%t2.a")' > %t1.lds
# RUN: ld.lld --fatal-warnings --warn-backrefs %t1.lds -o /dev/null

## A backward reference from %t1.o to %t2.a
# RUN: ld.lld --fatal-warnings %t2.a %t1.o -o /dev/null
# RUN: ld.lld --warn-backrefs %t2.a %t1.o -o /dev/null 2>&1 | FileCheck %s
# RUN: ld.lld --warn-backrefs %t2.a '-(' %t1.o '-)' -o /dev/null 2>&1 | FileCheck %s

## Placing the definition and the backward reference in a group can suppress the warning.
# RUN: echo 'GROUP("%t2.a" "%t1.o")' > %t2.lds
# RUN: ld.lld --fatal-warnings --warn-backrefs %t2.lds -o /dev/null
# RUN: ld.lld --fatal-warnings --warn-backrefs '-(' %t2.a %t1.o '-)' -o /dev/null

## A backward reference from %t1.o to %t2.a (added by %t3.lds).
# RUN: echo 'GROUP("%t2.a")' > %t3.lds
# RUN: ld.lld --warn-backrefs %t3.lds %t1.o -o /dev/null 2>&1 | FileCheck %s
# RUN: ld.lld --fatal-warnings --warn-backrefs '-(' %t3.lds %t1.o '-)' -o /dev/null
## If a lazy definition appears after the backward reference, don't warn.
# RUN: ld.lld --fatal-warnings --warn-backrefs %t3.lds %t1.o %t3.lds -o /dev/null

# CHECK: warning: backward reference detected: foo in {{.*}}1.o refers to {{.*}}2.a

## A backward reference from %t1.o to %t2.o
# RUN: ld.lld --warn-backrefs --start-lib %t2.o --end-lib %t1.o -o /dev/null 2>&1 | \
# RUN:   FileCheck --check-prefix=OBJECT %s
## If a lazy definition appears after the backward reference, don't warn.
# RUN: ld.lld --fatal-warnings --warn-backrefs --start-lib %t2.o --end-lib %t1.o --start-lib %t2.o --end-lib -o /dev/null

# OBJECT: warning: backward reference detected: foo in {{.*}}1.o refers to {{.*}}2.o

## Don't warn if the definition and the backward reference are in a group.
# RUN: echo '.globl bar; bar:' | llvm-mc -filetype=obj -triple=x86_64 - -o %t3.o
# RUN: echo '.globl foo; foo: call bar' | llvm-mc -filetype=obj -triple=x86_64 - -o %t4.o
# RUN: ld.lld --fatal-warnings --warn-backrefs %t1.o --start-lib %t3.o %t4.o --end-lib -o /dev/null

## We don't report backward references to weak symbols as they can be overridden later.
# RUN: echo '.weak foo; foo:' | llvm-mc -filetype=obj -triple=x86_64 - -o %tweak.o
# RUN: ld.lld --fatal-warnings --warn-backrefs --start-lib %tweak.o --end-lib %t1.o %t2.o -o /dev/null

## If a lazy definition appears after the backward reference, don't warn.
## A traditional Unix linker will resolve the reference to the later definition.
# RUN: ld.lld --fatal-warnings --warn-backrefs %t2.a %t1.o %t2.a -o /dev/null

## lld fetches the archive while GNU ld resolves the reference to the shared definition.
## Warn because the resolution rules are different.
# RUN: ld.lld --warn-backrefs %t2.a %t1.o %t2.so -o /dev/null 2>&1 | FileCheck %s

## This is a limitation. The resolution rules are different but
## --warn-backrefs does not warn.
# RUN: ld.lld --fatal-warnings --warn-backrefs %t2.a %t1.o %t2.so %t2.a -o /dev/null

.globl _start, foo
_start:
  call foo
