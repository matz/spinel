# A class named Object inside a module (`module M; class Object`) is M's
# own, and BasicObject's body is no top level: a module of a builtin's name
# in either is that namespace's, and the top-level builtin keeps its name.
module M
  class Object
    module Process
      def self.hello = :inner
    end
  end
end
p M::Object::Process.hello
p Process.pid.is_a?(Integer)
p Process.respond_to?(:hello)
class BasicObject
  module GC
    def self.hello = :basic
  end
end
p BasicObject::GC.hello
p GC.count.is_a?(Integer)
p GC.respond_to?(:hello)
