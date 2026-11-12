# A call in a lazy chain's block runs where the chain is forced, also where
# the chain is first chained on into another local (`once = pub.map { }`,
# then `once.first`): once or twice, through two locals or six, in
# parentheses. A root's def made before every call that forces it answers
# with the visibility there (Kernel's yield_self, then and tap made
# private), as in CRuby. A chain forced only before the visibility change
# finds the public def; a call in a block run at once, or in a branch,
# forces it where it is.
pub = [1].lazy.map { |_| begin; nil.yield_self { }; :public; rescue NoMethodError; :raised; end }
early = [1].lazy.map { |_| begin; nil.yield_self { }; :public; rescue NoMethodError; :raised; end }
twice = [1].lazy.map { |_| begin; nil.then { }; :public; rescue NoMethodError; :raised; end }
deep = [1].lazy.map { |_| begin; nil.yield_self { }; :public; rescue NoMethodError; :raised; end }
inner = [1].lazy.select { |_| begin; nil.tap { }; false; rescue NoMethodError; true; end }

module Kernel
  def yield_self; :k; end
  def then; :k; end
  def tap; :k; end
end

once = pub.map { |x| x }
seen = early.map { |x| x }
p seen.first
two = twice.map { |x| x }.select { |x| true }
three = two.reject { |x| false }
held = (inner.map { |x| x * 10 })
d1 = deep.map { |x| x }
d2 = d1.map { |x| x }
d3 = d2.map { |x| x }
d4 = d3.map { |x| x }
d5 = d4.map { |x| x }
d6 = d5.map { |x| x }

module Kernel
  private :yield_self, :then, :tap
end

p once.first
p three.first
p d6.first
[1].each { p held.first }
p(held.first ? held.to_a : [])
