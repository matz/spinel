# A top-level def is a private instance method of Object, and its
# visibility words at the top level apply to it: Object's
# method_defined? family answers for it from where the call runs.
p Object.private_method_defined?(:foo)
def foo(x) = x
p Object.private_method_defined?(:foo)
p Object.private_method_defined?(:foo, false)
p Object.method_defined?(:foo)
p Object.public_method_defined?(:foo)
p Object.protected_method_defined?(:foo)
p respond_to?(:foo, true)

def emit(x) = [:emit, x]
p Object.private_method_defined?(:emit)

public def pub(x) = x
p [Object.private_method_defined?(:pub), Object.public_method_defined?(:pub), Object.method_defined?(:pub)]

def later(x) = x
p Object.private_method_defined?(:later)
public :later
p [Object.private_method_defined?(:later), Object.public_method_defined?(:later, false)]

alias foo2 foo
p Object.private_method_defined?(:foo2)

# a top-level def of a Kernel name is Object's own
def puts(*a) = :mine
p [Object.private_method_defined?(:puts, false), Kernel.private_method_defined?(:puts, false)]

def gone(x) = x
undef gone
p Object.private_method_defined?(:gone)
