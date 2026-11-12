# A call in a lazy chain's block runs where the chain is forced: a root's
# def made between the block and every call forcing the chain (Kernel's
# private tap, Object's private then) answers it, as in CRuby; a chain
# forced only before the def finds the builtin's method. A block a method
# of the program keeps (`initialize(&b) = @b = b`) meets a later version of
# a program class's method when it runs; one a method only yields to runs
# at its point, as does an each block.
class Box
  def initialize(&b) = @b = b
  def keep(&b) = @k = b
  def run = @b.call
  def run_kept = @k.call
end

class Now
  def run_it = yield
end

class Ver
  def m = 1
end

early = [1].lazy.map { |_| begin; nil.tap { }; :public; rescue NoMethodError => e; e.message; end }
p early.first
lazy = [1].lazy.map { |_| begin; nil.tap { }; :public; rescue NoMethodError => e; e.message; end }
lazy_int = [1].lazy.select { |_| begin; 1.then { }; false; rescue NoMethodError => e; p e.message; true; end }
made = Box.new { Ver.new.m }
kept = Box.new { }
kept.keep { Ver.new.m * 10 }
Now.new.run_it { p(begin; nil.tap { }; :public; rescue NoMethodError => e; e.message; end) }
xs = [1]
xs.each { p(begin; nil.tap { }; :public; rescue NoMethodError => e; e.message; end) }
p made.run

module Kernel
  private
  def tap; :k; end
end

class Object
  private
  def then; :o; end
end

class Ver
  def m = 2
end

p lazy.first
p lazy_int.to_a
p made.run
p kept.run_kept
Now.new.run_it { p(begin; nil.tap { }; :public; rescue NoMethodError => e; e.message; end) }
xs.each { p(begin; nil.tap { }; :public; rescue NoMethodError => e; e.message; end) }
