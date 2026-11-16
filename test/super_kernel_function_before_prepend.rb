# A call made before a module is prepended, of a name only Kernel has as a
# private function, raises the private-method NoMethodError, as it has no
# method of the class's yet: a super in the stand-in the call's version entry
# runs does not make it a visibility-blind call of Kernel's.
module P
  def puts(x) = :p
end
class C
end
o = C.new
begin
  p o.puts("hi")
rescue NoMethodError => e
  p e.class
end
C.prepend(P)
p o.puts("hi")
