# Text a macro runs in a module's body is the body's code, as in a class's:
# the def it writes is the module's, and `extend self` makes it the
# module's own method too, as CRuby answers.
module Macros
  def reader(name) = module_eval("def #{name} = :#{name}")
end
module Helpers
  extend Macros
  extend self
  reader :help
end
p Helpers.help
