# A visibility word with a name a method's body may make: `private :zz, :tail` with zz made under a condition the run decides.
# CRuby changes each name in turn and raises NameError at the first that no method has, so the names after it keep their
# visibility. The word tests the version cells of such a name where it runs, raises there, and writes the cells of the names
# it passes as it goes; a call, respond_to?, the lists and the method_defined? kin of the later names read those cells.
# Each case has its own class and its own definer method, one run with the name absent and, in another case, one with it present.

# 1: class_eval block def under `if` in a class method; reopen's rescued private; mk(false)
puts "1"
class C1
  def self.mk1(flag)
    C1.class_eval { def zz; :zz; end } if flag
  end
  def tail = :tail
end
C1.mk1(false)
class C1
  begin; private :zz, :tail; rescue NameError; end
end
c1 = C1.new
p [c1.respond_to?(:zz), (c1.tail rescue $!.class)]

# 2: class_eval block def under `if` in a class method; reopen's rescued private; mk(true)
puts "2"
class C2
  def self.mk2(flag)
    C2.class_eval { def zz; :zz; end } if flag
  end
  def tail = :tail
end
C2.mk2(true)
class C2
  begin; private :zz, :tail; rescue NameError; end
end
c2 = C2.new
p [c2.respond_to?(:zz), (c2.tail rescue $!.class)]

# 3: def directly under `if` in a class method body; mk(false)
puts "3"
class C3
  def self.mk3(f) = (def zz = :zz if f)
  def tail = :tail
end
C3.mk3(false)
class C3
  begin; private :zz, :tail; rescue NameError; end
end
c3 = C3.new
p [c3.respond_to?(:zz), (c3.tail rescue $!.class)]

# 4: def directly under `if` in a class method body; mk(true)
puts "4"
class C4
  def self.mk4(f) = (def zz = :zz if f)
  def tail = :tail
end
C4.mk4(true)
class C4
  begin; private :zz, :tail; rescue NameError; end
end
c4 = C4.new
p [c4.respond_to?(:zz), (c4.tail rescue $!.class)]

# 5: def in a rescued begin of a method body, after a statement that may raise; mk(false)
puts "5"
class C5
  def self.mk5(f)
    begin
      raise "no" unless f
      def zz = :zz
    rescue RuntimeError
    end
  end
  def tail = :tail
end
C5.mk5(false)
class C5
  begin; private :zz, :tail; rescue NameError; end
end
c5 = C5.new
p [c5.respond_to?(:zz), (c5.tail rescue $!.class)]

# 6: def in a rescued begin of a method body, after a statement that may raise; mk(true)
puts "6"
class C6
  def self.mk6(f)
    begin
      raise "no" unless f
      def zz = :zz
    rescue RuntimeError
    end
  end
  def tail = :tail
end
C6.mk6(true)
class C6
  begin; private :zz, :tail; rescue NameError; end
end
c6 = C6.new
p [c6.respond_to?(:zz), (c6.tail rescue $!.class)]

# 9: two may-names: a present, b absent
puts "9"
class C9
  def self.mk9(fa, fb)
    C9.class_eval { def ma; :ma; end } if fa
    C9.class_eval { def mb; :mb; end } if fb
  end
  def tail = :tail
end
C9.mk9(true, false)
class C9
  begin; private :ma, :mb, :tail; rescue NameError; end
end
c9 = C9.new
p [(c9.ma rescue $!.class), (c9.mb rescue $!.class), (c9.tail rescue $!.class)]

# 10: two may-names: both present
puts "10"
class C10
  def self.mk10(fa, fb)
    C10.class_eval { def ma; :ma; end } if fa
    C10.class_eval { def mb; :mb; end } if fb
  end
  def tail = :tail
end
C10.mk10(true, true)
class C10
  begin; private :ma, :mb, :tail; rescue NameError; end
end
c10 = C10.new
p [(c10.ma rescue $!.class), (c10.mb rescue $!.class), (c10.tail rescue $!.class)]

# 11: two may-names: a absent, b present
puts "11"
class C11
  def self.mk11(fa, fb)
    C11.class_eval { def ma; :ma; end } if fa
    C11.class_eval { def mb; :mb; end } if fb
  end
  def tail = :tail
end
C11.mk11(false, true)
class C11
  begin; private :ma, :mb, :tail; rescue NameError; end
end
c11 = C11.new
p [(c11.ma rescue $!.class), (c11.mb rescue $!.class), (c11.tail rescue $!.class)]

# 12: three may-names, all present, then a sure name; then a run with one absent on another class
puts "12"
class C12
  def self.mk12(f) = (C12.class_eval { def ma; :ma; end; def mb; :mb; end; def md; :md; end } if f)
  def tail = :tail
end
class D12
  def self.mk12(f) = (D12.class_eval { def ma; :ma; end; def md; :md; end } if f)
  def self.mkb12(f) = (D12.class_eval { def mb; :mb; end } if f)
  def tail = :tail
end
C12.mk12(true); D12.mk12(true); D12.mkb12(false)
class C12; begin; private :ma, :mb, :md, :tail; rescue NameError; end; end
class D12; begin; private :ma, :mb, :md, :tail; rescue NameError; end; end
p [C12.new.respond_to?(:ma), C12.new.respond_to?(:md), (C12.new.tail rescue $!.class)]
p [D12.new.respond_to?(:ma), D12.new.respond_to?(:md), (D12.new.tail rescue $!.class)]

# 13: public word: tail private before; public :zz, :tail with zz absent leaves tail private
puts "13"
class C13
  def self.mk13(f) = (C13.class_eval { def zz; :zz; end } if f)
  private def tail = :tail
end
C13.mk13(false)
class C13
  begin; public :zz, :tail; rescue NameError; end
end
p [(C13.new.tail rescue $!.class), C13.new.respond_to?(:tail)]

# 14: public word: tail private before; public :zz, :tail with zz present makes tail public
puts "14"
class C14
  def self.mk14(f) = (C14.class_eval { def zz; :zz; end } if f)
  private def tail = :tail
end
C14.mk14(true)
class C14
  begin; public :zz, :tail; rescue NameError; end
end
p [(C14.new.tail rescue $!.class), C14.new.respond_to?(:tail)]

# 15: protected word, zz absent: tail stays public
puts "15"
class C15
  def self.mk15(f) = (C15.class_eval { def zz; :zz; end } if f)
  def tail = :tail
end
C15.mk15(false)
class C15
  begin; protected :zz, :tail; rescue NameError; end
end
p [(C15.new.tail rescue $!.class), C15.protected_method_defined?(:tail)]

# 16: protected word, zz present: tail protected
puts "16"
class C16
  def self.mk16(f) = (C16.class_eval { def zz; :zz; end } if f)
  def tail = :tail
end
C16.mk16(true)
class C16
  begin; protected :zz, :tail; rescue NameError; end
end
p [(C16.new.tail rescue $!.class), C16.protected_method_defined?(:tail)]

# 23: send form of the word, in the reopen's body; zz absent
puts "23"
class C23
  def self.mk23(f) = (C23.class_eval { def zz; :zz; end } if f)
  def tail = :tail
end
C23.mk23(false)
class C23
  begin; send(:private, :zz, :tail); rescue NameError; end
end
p [(C23.new.zz rescue $!.class), (C23.new.tail rescue $!.class)]

# 24: send form of the word, in the reopen's body; zz present
puts "24"
class C24
  def self.mk24(f) = (C24.class_eval { def zz; :zz; end } if f)
  def tail = :tail
end
C24.mk24(true)
class C24
  begin; send(:private, :zz, :tail); rescue NameError; end
end
p [(C24.new.zz rescue $!.class), (C24.new.tail rescue $!.class)]

# 28: readers of tail's visibility: respond_to?, public_send, send, method_defined lists; zz absent
puts "28"
class C28
  def self.mk28(f) = (C28.class_eval { def zz; :zz; end } if f)
  def tail = :tail
end
C28.mk28(false)
class C28
  begin; private :zz, :tail; rescue NameError; end
end
c28 = C28.new
p [c28.respond_to?(:tail), (c28.public_send(:tail) rescue $!.class), c28.send(:tail),
   C28.public_method_defined?(:tail), C28.private_instance_methods(false).include?(:tail), C28.public_instance_methods(false).sort]

# 29: readers of tail's visibility: respond_to?, public_send, send, method_defined lists; zz present
puts "29"
class C29
  def self.mk29(f) = (C29.class_eval { def zz; :zz; end } if f)
  def tail = :tail
end
C29.mk29(true)
class C29
  begin; private :zz, :tail; rescue NameError; end
end
c29 = C29.new
p [c29.respond_to?(:tail), (c29.public_send(:tail) rescue $!.class), c29.send(:tail),
   C29.public_method_defined?(:tail), C29.private_instance_methods(false).include?(:tail), C29.public_instance_methods(false).sort]

# 30: Array operand: private [:zz, :tail], zz absent
puts "30"
class C30
  def self.mk30(f) = (C30.class_eval { def zz; :zz; end } if f)
  def tail = :tail
end
C30.mk30(false)
class C30
  begin; private [:zz, :tail]; rescue NameError; end
end
p [(C30.new.tail rescue $!.class)]

# 31: Array operand: private [:zz, :tail], zz present
puts "31"
class C31
  def self.mk31(f) = (C31.class_eval { def zz; :zz; end } if f)
  def tail = :tail
end
C31.mk31(true)
class C31
  begin; private [:zz, :tail]; rescue NameError; end
end
p [(C31.new.tail rescue $!.class)]

# 36: the NameError the word raises, zz absent; and no raise, zz present
puts "36"
class C36
  def self.mk36(f) = (C36.class_eval { def zz; :zz; end } if f)
  def tail = :tail
end
C36.mk36(false)
class C36
  r = begin; private :zz, :tail; :ran; rescue NameError => e; e.message; end
  p r
end
p (C36.new.tail rescue $!.class)

# 41: a sure name ahead of the may-name: it is private though the word raises at zz; tail is not
puts "41"
class C41
  def self.mk41(f) = (C41.class_eval { def zz; :zz; end } if f)
  def ma = :ma
  def tail = :tail
end
C41.mk41(false)
class C41
  begin; private :ma, :zz, :tail; rescue NameError; end
end
p [(C41.new.ma rescue $!.class), (C41.new.tail rescue $!.class)]

# 42: a sure name ahead of the may-name: zz present: all three private
puts "42"
class C42
  def self.mk42(f) = (C42.class_eval { def zz; :zz; end } if f)
  def ma = :ma
  def tail = :tail
end
C42.mk42(true)
class C42
  begin; private :ma, :zz, :tail; rescue NameError; end
end
p [(C42.new.ma rescue $!.class), (C42.new.tail rescue $!.class)]

# The same word where the call that makes the name does not surely run (an iterator's block that may not run, `for`, `case/in`, an
# early return or next ahead of it, `0.times`): the name is tested by its cell.
YES = !ENV["SPINEL_NO_SUCH_VARIABLE_FOR_THIS_TEST"]

puts "q01_each_empty"
class F100
  def self.mk100(xs) = xs.each { F100.class_eval { def zz = :zz } }
  def tail = :tail

end
F100.mk100([])
class F100
  begin; private :zz, :tail; rescue NameError => e; p e.message; end
end

c100 = F100.new
p [c100.respond_to?(:zz), (c100.zz rescue $!.class), (c100.tail rescue $!.class), F100.private_instance_methods(false).sort, F100.public_method_defined?(:tail)]

puts "q03_return_ahead_f"
class F101
  def self.mk101(f)
    return unless f
    F101.class_eval { def zz = :zz }
  end
  def tail = :tail

end
F101.mk101(false)
class F101
  begin; private :zz, :tail; rescue NameError => e; p e.message; end
end

c101 = F101.new
p [c101.respond_to?(:zz), (c101.zz rescue $!.class), (c101.tail rescue $!.class), F101.private_instance_methods(false).sort, F101.public_method_defined?(:tail)]

puts "q05_casein_f"
class F102
  def self.mk102(f)
    case f
    in true then F102.class_eval { def zz = :zz }
    else nil
    end
  end
  def tail = :tail

end
F102.mk102(false)
class F102
  begin; private :zz, :tail; rescue NameError => e; p e.message; end
end

c102 = F102.new
p [c102.respond_to?(:zz), (c102.zz rescue $!.class), (c102.tail rescue $!.class), F102.private_instance_methods(false).sort, F102.public_method_defined?(:tail)]

puts "q06_for_empty"
class F103
  def self.mk103(xs)
    for i in xs
      F103.class_eval { def zz = :zz }
    end
  end
  def tail = :tail

end
F103.mk103([])
class F103
  begin; private :zz, :tail; rescue NameError => e; p e.message; end
end

c103 = F103.new
p [c103.respond_to?(:zz), (c103.zz rescue $!.class), (c103.tail rescue $!.class), F103.private_instance_methods(false).sort, F103.public_method_defined?(:tail)]

puts "q08_next_in_block"
class F104
  def self.mk104(f) = [1].each { next unless f; F104.class_eval { def zz = :zz } }
  def tail = :tail

end
F104.mk104(false)
class F104
  begin; private :zz, :tail; rescue NameError => e; p e.message; end
end

c104 = F104.new
p [c104.respond_to?(:zz), (c104.zz rescue $!.class), (c104.tail rescue $!.class), F104.private_instance_methods(false).sort, F104.public_method_defined?(:tail)]

puts "q09_times_zero"
class F105
  def self.mk105(n) = n.times { F105.class_eval { def zz = :zz } }
  def tail = :tail

end
F105.mk105(0)
class F105
  begin; private :zz, :tail; rescue NameError => e; p e.message; end
end

c105 = F105.new
p [c105.respond_to?(:zz), (c105.zz rescue $!.class), (c105.tail rescue $!.class), F105.private_instance_methods(false).sort, F105.public_method_defined?(:tail)]

# A name a condition provides (an include or an attribute under an `if`) is no name a bare word finds missing; and another class's
# conditional word does not close this class's public_method.
NO = !YES

puts "bare word, name from a conditional include"
module M200; def t2 = :t2; end
class C200
  def self.mk200(f)
    C200.class_eval { def zz = :zz } if f
  end
  include M200 if YES
  def tail = :tail
end
C200.mk200(true)
class C200
  private :zz, :t2, :tail
end
c200 = C200.new
p [(c200.t2 rescue $!.class), (c200.tail rescue $!.class), c200.respond_to?(:zz)]

puts "name from a conditional attribute"
module M202; def t2 = :t2; end
class C202
  def self.mk202(f)
    C202.class_eval { def zz = :zz } if f
  end
  attr_reader :t2 if YES
  def tail = :tail
end
C202.mk202(true)
begin
  class C202
    private :zz, :t2, :tail
  end
rescue NameError => e
  p e.message
end
c202 = C202.new
p [(c202.t2 rescue $!.class), (c202.tail rescue $!.class), c202.respond_to?(:zz)]

puts "public_method of another class"
class K203; def tail = :k; private :tail if NO; end
class C203
  def self.mk203(f)
    C203.class_eval { def zz = :zz } if f
  end
  def tail = :tail
end
C203.mk203(true)
class C203
  begin; private :zz, :tail; rescue NameError => e; p e.message; end
end

p [(K203.new.public_method(:tail).call rescue $!.class), (K203.new.tail rescue $!.class), (C203.new.public_method(:tail).call rescue $!.class)]

# A public_method of a name after a gated word reads the word as run only where the def it binds is the one the word changes:
# not a module's conditional word on the lookup, and not a conditional word of the def's own ahead of it.

puts "public_method through a module with its own conditional word"
module N300; def tail = :n; private :tail if NO; end
class C300
  def self.mk300(f)
    C300.class_eval { def zz = :zz } if f
  end
  def tail = :tail
end
class D300 < C300; include N300; end
C300.mk300(true)
class C300
  begin; private :zz, :tail; rescue NameError => e; p e.message; end
end
p [(D300.new.public_method(:tail).call rescue $!.class), (C300.new.public_method(:tail).call rescue $!.class)]

puts "a conditional word ahead of the gated word"
class C301
  def self.mk301(f)
    C301.class_eval { def zz = :zz } if f
  end
  def tail = :tail
end
class C301; private :tail if NO; end
C301.mk301(true)
class C301
  begin; private :zz, :tail; rescue NameError => e; p e.message; end
end
p [(C301.new.public_method(:tail).call rescue $!.class), (C301.new.tail rescue $!.class), (C301.new.public_method(:tail).call rescue $!.class)]
