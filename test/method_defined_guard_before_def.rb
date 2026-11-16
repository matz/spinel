# A method_defined? guard asks where it stands: a def made later in the file
# (a reopening below it) is not there yet, so the guard's else arm and a
# polyfill body under `unless` run, and the later def replaces them. A guard
# inside a method answers from the table.
class C; end
if C.method_defined?(:m)
  puts "defined"
else
  puts "absent"
end
unless C.method_defined?(:pf)
  class C; def pf = :poly; end
end
p C.new.pf
class D
  def hello = :first unless method_defined?(:hello)
end
p D.new.hello
class C
  def m = 1
  def pf = :real
end
class D; def hello = :second; end
puts(C.method_defined?(:m) ? "defined" : "absent")
p C.new.pf, D.new.hello
class E; end
def probe_e = (E.method_defined?(:late) ? :has : :none)
class E; def late = 1; end
p probe_e
