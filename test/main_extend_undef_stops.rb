# An undef in a module main extends stops the lookup of a call on main, as
# main's own undef does: Object's, Kernel's and the top-level method behind
# it do not answer.

class Object; def f1 = "Object#f1"; end
module M1; def f1 = "M1#f1"; undef_method :f1; end
extend M1
p((f1 rescue $!.class))
p((self.f1 rescue $!.class))
p((send(:f1) rescue $!.class))
[1].each { p((f1 rescue $!.class)) }
p(("#{f1}" rescue $!.class))

# a Kernel method and a top-level def behind it
module Kernel; def f2 = "Kernel#f2"; end
def f3(x) = "top f3 #{x}"
module M2; def f2 = "M2#f2"; def f3(x) = "M2#f3"; undef_method :f2; undef f3; end
extend M2
p((f2 rescue $!.class))
p((f3(1) rescue $!.class))

# the undef in a module the extended one includes
class Object; def f4 = "Object#f4"; end
module N4; def f4 = "N4#f4"; undef_method :f4; end
module M4; include N4; end
extend M4
p((f4 rescue $!.class))

# the later extend comes first
class Object; def f5 = "Object#f5"; end
module A5; def f5 = "A5#f5"; end
module B5; def f5 = "B5#f5"; undef_method :f5; end
extend A5
extend B5
p((f5 rescue $!.class))

# main's own method defined after the extend answers ahead of it
class Object; def f6 = "Object#f6"; end
module M6; def f6 = "M6#f6"; undef_method :f6; end
extend M6
p((f6 rescue $!.class))
def self.f6 = "main.f6"
p((f6 rescue $!.class))

# a module main does not extend leaves Object's method
class Object; def f7 = "Object#f7"; end
module M7; def f7 = "M7#f7"; undef_method :f7; end
p((f7 rescue $!.class))
p((nil.f1 rescue $!.class))

# main's own methods (private, include, ...) stand ahead of the extends
module M8; def private(*a) = :m8; undef_method :private; end
extend M8
def foo8 = 8
p((private(:foo8) rescue $!.class))

# a repeated extend is no new one: the later extend stays first
class Object; def f9 = "Object#f9"; end
module A9; def f9 = "A9#f9"; end
module B9; def f9 = "B9#f9"; undef_method :f9; end
extend A9
extend B9
extend A9
p((f9 rescue $!.class))

# an extend a raise the rescue takes may have skipped
class Object; def f10 = "Object#f10"; end
module M10; def f10 = "M10#f10"; undef_method :f10; end
begin
  Integer("zz")
  extend M10
rescue ArgumentError
  p :argument_error
end
p((f10 rescue $!.class))
