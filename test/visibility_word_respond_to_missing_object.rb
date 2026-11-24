# A visibility word tests the methods of a name the run decides, whatever a respond_to_missing? of the program answers. The hook returns
# false on Object; zz is skipped (the word raises NameError at it and leaves tail public) and made (the word passes). A program with a
# second hook reads the first one's word differently, so each placement has a file of its own.

puts "on Object, zz skipped and made"
class Object; def respond_to_missing?(n, all = false) = false; end
class C314
  def self.mk314(f) = (C314.class_eval { def zz = :zz } if f)
  def tail = :tail
end
class C315
  def self.mk315(f) = (C315.class_eval { def zz = :zz } if f)
  def tail = :tail
end
C314.mk314(false)
C315.mk315(true)
class C314
  r = begin; private :zz, :tail; :ran; rescue NameError => e; e.message; end
  p r
end
class C315
  r = begin; private :zz, :tail; :ran; rescue NameError => e; e.message; end
  p r
end
p [(C314.new.tail rescue $!.class), (C314.new.zz rescue $!.class), (C315.new.tail rescue $!.class), (C315.new.zz rescue $!.class)]
