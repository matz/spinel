# spinel: not-cruby -- CRuby answers these three supers with the top-level def (which the program makes after the class), Spinel keeps its NoMethodError
# A top-level def is Object's private method, ahead of Kernel's, for a
# chain that reaches Object. A `super` into a Kernel function that the
# program also defines at the top level, made after the class, cannot be
# proven to reach either: the record cannot say the def has run. It was
# answered by Kernel's function, which is wrong wherever the def has run
# (CRuby: "<T>"); it keeps the NoMethodError it had instead of that answer.
class Foo
  def format(f, n) = "<" + super + ">"
end

module M
  def format(f, n) = "<" + super + ">"
end
class A; include M; end
class B; include M; end

class Arr < Array
  def format(f, n) = super
end

def format(f, n) = "T"

[-> { Foo.new.format("%d", 1) },
 -> { A.new.format("%d", 1) },
 -> { B.new.format("%d", 2) },
 -> { Arr.new.format("%d", 1) }].each do |t|
  begin
    p t.call
  rescue NoMethodError => e
    puts "NoMethodError #{e.message[/superclass method '\w+'/]}"
  end
end
