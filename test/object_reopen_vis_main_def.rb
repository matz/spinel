# A visibility word in a reopened Object changes a top-level def's
# visibility, which Object's method_defined? family reads where it runs.
public
def a1; end
p Object.private_method_defined?(:a1)
class Object; private :a1; end
p Object.method_defined?(:a1), Object.private_method_defined?(:a1)
public :a1
p Object.method_defined?(:a1), Object.private_method_defined?(:a1)
class Object; private :a1; end
def a1; end
p Object.method_defined?(:a1), Object.private_method_defined?(:a1)

def a2; end
class Object; public :a2; protected :a2; end
p Object.protected_method_defined?(:a2), Object.method_defined?(:a2), Object.public_method_defined?(:a2)

def a4; end
class Object; public :a4; end
p Object.public_method_defined?(:a4, false), Object.private_method_defined?(:a4, false)
