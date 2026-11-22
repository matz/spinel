# const_defined? finds the constants of a builtin class or module that a
# path such as Process::WNOHANG reads, as defined? does.
p Process.const_defined?(:WNOHANG)
p Process.const_defined?(:CLOCK_MONOTONIC)
p Process.const_defined?("WUNTRACED")
p Process.const_defined?(:Status)
p Process.const_defined?(:RLIMIT_NOFILE) == !!defined?(Process::RLIMIT_NOFILE)
p Process.const_defined?(:NoSuchThing)
p Math.const_defined?(:PI)
p Math.const_defined?(:DomainError)
p Float.const_defined?(:INFINITY)
p Float.const_defined?(:DIG)
p File.const_defined?(:SEPARATOR)
p File.const_defined?(:SEEK_SET)
p Process.const_defined?(:WNOHANG, false)
p Math.const_defined?(:PI, false)
p File.const_defined?(:SEEK_SET, false)
p IO.const_defined?(:SEEK_SET, false)
module Calc
  include Math
end
p Calc.const_defined?(:PI)
p Calc.const_defined?(:PI, false)
p File.const_defined?(:SEPARATOR, false)
p File.const_defined?(:RDONLY, false)
p File.const_defined?(:FNM_PATHNAME, false)
p Process.const_defined?(:RLIM_INFINITY)
p Process.const_defined?(:RLIMIT_XYZ)
p Float.const_defined?(:PI)
p Encoding.const_defined?(:UTF8)

# a class of the program named like a builtin has its own constants alone
module Foo
  module Process; end
  class Float; end
  module Math; end
  def self.a = Process.const_defined?(:WNOHANG)
  def self.b = Float.const_defined?(:INFINITY)
  def self.c = Math.const_defined?(:PI)
end
p Foo.a, Foo.b, Foo.c
p Foo::Process.const_defined?(:Status)
p Foo::Float.const_defined?(:DIG)
p Foo::Math.const_defined?(:PI)
module Bar
  module Math
    include ::Math
  end
end
p Bar::Math.const_defined?(:PI)

# a reopened builtin keeps the builtin's constants
module Process
  MINE = 1
end
p Process.const_defined?(:MINE), Process.const_defined?(:CLOCK_REALTIME)
