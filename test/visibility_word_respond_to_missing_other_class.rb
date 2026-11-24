# A visibility word tests the methods of a name the run decides, whatever a respond_to_missing? of the program answers. The hook returns
# false on another class; zz is skipped (the word raises NameError at it and leaves tail public) and made (the word passes). A program with a
# second hook reads the first one's word differently, so each placement has a file of its own.

puts "on another class, zz skipped and made"
class Hook312; def respond_to_missing?(n, all = false) = false; end
class C312
  def self.mk312(f) = (C312.class_eval { def zz = :zz } if f)
  def tail = :tail
end
class C313
  def self.mk313(f) = (C313.class_eval { def zz = :zz } if f)
  def tail = :tail
end
C312.mk312(false)
C313.mk313(true)
class C312
  r = begin; private :zz, :tail; :ran; rescue NameError => e; e.message; end
  p r
end
class C313
  r = begin; private :zz, :tail; :ran; rescue NameError => e; e.message; end
  p r
end
p [(C312.new.tail rescue $!.class), (C312.new.zz rescue $!.class), (C313.new.tail rescue $!.class), (C313.new.zz rescue $!.class)]
