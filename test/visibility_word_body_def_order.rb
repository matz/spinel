# A visibility word in a rescued begin reaches the names a method's body made before it ran, in the order the bodies run.
class Q1
  def self.mk1 = (def zz1 = :zz1; nil)
  def a1 = :a1
end
Q1.mk1
class Q1
  begin; private :a1, :zz1, :missing; rescue NameError; end
end
k1 = Q1.new
p [(k1.a1 rescue $!.class), (k1.zz1 rescue $!.class), (k1.send(:zz1) rescue $!.class)]
class Q2
  def self.mk2 = (def zz2 = :zz2; nil)
  def a2 = :a2
end
Q2.mk2 unless ARGV.empty?
class Q2
  begin; private :a2, :zz2, :missing; rescue NameError; end
end
k2 = Q2.new
p [(k2.a2 rescue $!.class), (k2.zz2 rescue $!.class), (k2.send(:zz2) rescue $!.class)]
class Q3
  def self.mk3 = (def zz3 = :zz3; nil)
  def a3 = :a3
end
Q3.mk3
class Q3
  private :zz3
end
r3 = [(Q3.new.zz3 rescue $!.class)]
Q3.mk3
r3 << (Q3.new.zz3 rescue $!.class)
p r3
class Q4
  def self.mk4 = (def zz4 = :zz4; nil)
  def a4 = :a4
end
class Q4
  def zz4 = :old4
end
Q4.mk4
class Q4
  begin; private :a4, :zz4, :missing; rescue NameError; end
end
k4 = Q4.new
p [(k4.a4 rescue $!.class), (k4.zz4 rescue $!.class), (k4.send(:zz4) rescue $!.class)]
class R5
  def self.mk5 = define_method(:b5) { :b5 }
  def a5 = :a5
  mk5
  begin
    private :b5, :a5
  rescue NameError
  end
end
p [(R5.new.a5 rescue $!.class), (R5.new.b5 rescue $!.class)]
class R6
  def a6 = :a6
  begin
    private def x = :x, :zz6, :a6
  rescue NameError
  end
  def self.mk6 = define_method(:zz6) { :zz6 }
end
R6.mk6
p [(R6.new.a6 rescue $!.class), (R6.new.zz6 rescue $!.class)]
class S7
  def self.mk7 = R7.class_eval { def b7 = :b7 }
end
class R7
  def a7 = :a7
end
S7.mk7
class R7
  def c7 = :c7
  begin
    private :a7, :b7, :c7, :zz7
  rescue NameError
  end
end
k7 = R7.new
p [(k7.a7 rescue $!.class), (k7.b7 rescue $!.class), (k7.c7 rescue $!.class)]
class R8
  def a8 = :a8
  def self.mk8 = (def zz8 = :zz8)
  mk8
  begin; private :zz8, :a8; rescue NameError; end
end
p [(R8.new.a8 rescue $!.class), (R8.new.zz8 rescue $!.class)]
class S9
  def self.mk9 = R9.class_eval { def zz9 = :zz9 }
end
class R9; end
S9.mk9
class R9
  def a9 = :a9
  def b9 = :b9
  begin
    private :a9, :zz9, :b9
  rescue NameError
  end
  alias b29 b9
end
p [(R9.new.b9 rescue $!.class), (R9.new.b29 rescue $!.class), (R9.new.a9 rescue $!.class)]
class S10
  def self.mk10 = (def zz10 = :zz10)
end
class R10 < S10
  def a10 = :a10
  def b10 = :b10
  begin
    private :a10, :zz10, :b10
  rescue NameError
  end
end
S10.mk10
k10 = R10.new
p [(k10.a10 rescue $!.class), (k10.zz10 rescue $!.class), (k10.b10 rescue $!.class)]
module M11
  def self.mk11 = (def zz11 = :zz11)
end
class R11
  include M11
  def a11 = :a11
  def b11 = :b11
  begin
    private :a11, :zz11, :b11
  rescue NameError
  end
end
M11.mk11 if ARGV.size > 5
M11.mk11
k11 = R11.new
p [(k11.a11 rescue $!.class), (k11.zz11 rescue $!.class), (k11.b11 rescue $!.class), R11.private_method_defined?(:zz11)]
class R12
  define_method(:dm12) { :dm12 }
  def a12 = :a12
  begin; private :a12, :dm12, :zz12, :a12; rescue NameError => e12; p e12.class; end
end
p [(R12.new.a12 rescue $!.class), (R12.new.dm12 rescue $!.class)]
module M14; def self.mk14 = (def zz14 = :zz14); end
M14.mk14
class C14
  def tail14 = :tail14
  begin; private :zz14, :tail14; rescue NameError; end
  include M14
end
p [(C14.new.tail14 rescue $!.class), (C14.new.zz14 rescue $!.class)]
class C15
  def self.mk15 = (def zz15 = :zz15)
  mk15
  begin; private(:zz15, def tail15; :tail15; end); rescue NameError; end
end
c15 = C15.new
p [(c15.zz15 rescue $!.class), (c15.tail15 rescue $!.class)]
