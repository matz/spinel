# defined? evaluates the parent of a scoped path, so an autoload there runs;
# the leaf alone is only observed, without loading.
autoload :DefParent, File.join(__dir__, "autoload_defined_scoped_lib")
autoload :DefLeaf, File.join(__dir__, "autoload_defined_scoped_leaf_lib")
puts "main"
p defined?(DefLeaf)
p defined?(DefParent::Inner)
p defined?(DefParent::Missing)
p defined?(DefLeaf)
p DefLeaf.v
