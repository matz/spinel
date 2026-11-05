# `K.include(M)` / `K.prepend(M)` at the top level or in another body,
# after a call on K: the call ahead of it answers without the module, the
# ones after it with it (the load record's mixin, guarded from its
# statement as a reopening's is) (#10790).
module Greet; def hi = :greet; end
class K; end
p((K.new.hi rescue :none))
module Setup; K.include(Greet); end
p K.new.hi

module Wrap; def name = "W>" + super; end
class Item; def name = "item"; end
it = Item.new
p it.name
Item.prepend(Wrap)
p it.name, Item.new.name, Item.ancestors.first

module Plug; def plug = :plug; end
class App; end
App.include(Plug)
App.send(:include, Comparable)
p App.new.plug, App.include?(Plug), App.include?(Comparable)
