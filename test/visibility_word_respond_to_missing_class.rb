# A visibility word tests the methods of a name the run decides, whatever a respond_to_missing? of the program answers. The hook returns
# false on the class; zz is skipped (the word raises NameError at it and leaves tail public) and made (the word passes). A program with a
# second hook reads the first one's word differently, so each placement has a file of its own.

puts "on the class, zz skipped and made"
class C310
  def self.mk310(f) = (C310.class_eval { def zz = :zz } if f)
  def tail = :tail
  def respond_to_missing?(n, all = false) = false
end
class C311
  def self.mk311(f) = (C311.class_eval { def zz = :zz } if f)
  def tail = :tail
  def respond_to_missing?(n, all = false) = false
end
C310.mk310(false)
C311.mk311(true)
class C310
  r = begin; private :zz, :tail; :ran; rescue NameError => e; e.message; end
  p r
end
class C311
  r = begin; private :zz, :tail; :ran; rescue NameError => e; e.message; end
  p r
end
p [(C310.new.tail rescue $!.class), (C310.new.zz rescue $!.class), (C311.new.tail rescue $!.class), (C311.new.zz rescue $!.class)]
