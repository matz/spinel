# An entry in a reopened Object ahead of a top-level entry of the same
# name. A top-level def is private. A top-level alias takes the
# visibility of its source method.

# an alias in Object, then a top-level def: the def is the method
class Object; alias_method :to_s, :inspect; end
def to_s; "m"; end
p Object.method_defined?(:to_s), Object.private_method_defined?(:to_s)

# an alias in Object, then a top-level alias of a public method
class Object; alias_method :inspect, :to_s; end
alias inspect hash
p Object.method_defined?(:inspect), Object.private_method_defined?(:inspect)
