# A module of a builtin module's name nested in another (M::Process,
# M::Object::Process) is a module of its own beside a top-level reopen of
# the builtin: both keep their methods, and the builtin's own still answer.
# The two were given one C name, and the C did not compile.
module Process
  def self.extra = :top
end
module M
  module Process
    def self.hello = :inner
  end
  class Object
    module Process
      def self.hello = :deeper
    end
  end
end
p M::Process.hello
p M::Object::Process.hello
p Process.extra
p Process.pid.is_a?(Integer)
p M::Process.respond_to?(:extra)
p Process.respond_to?(:hello)
