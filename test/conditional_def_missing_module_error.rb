# A module that defines a NoMethodError of its own does not capture the
# error a method left undefined by a run-time condition raises: it is
# ::NoMethodError, with the method's name, its receiver and its arguments,
# also when the module's class has an initialize of its own.
module M
  class NoMethodError < StandardError
    def initialize(m) = super("M:" + m)
  end
  class C
    if ARGV.size > 5
      def late(x) = x
    end
  end
  if ARGV.size > 5
    def self.top = 1
  end
end
begin; M::C.new.late(4)
rescue ::NoMethodError => e then p [:top, e.name, e.args, e.receiver.class]
rescue M::NoMethodError => e then p [:m, e.message]
rescue ArgumentError => e then p [:arg, e.message]
end
begin; M.top
rescue ::NoMethodError => e then p [:top, e.name, e.args, e.receiver]
rescue M::NoMethodError => e then p [:m, e.message]
end
