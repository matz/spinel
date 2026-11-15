# A define_method at the top level defines a public method of Object
# whatever visibility section it stands in (CRuby makes main's own
# define_method public, unlike a class body's), so the method_defined?
# family answers public for it, and private again once a word says so.
define_method(:dm_plain) { 1 }
p Object.public_method_defined?(:dm_plain)
p Object.private_method_defined?(:dm_plain)
p Object.method_defined?(:dm_plain)

private
define_method(:dm_after_private) { 2 }
public
p Object.public_method_defined?(:dm_after_private)
p Object.private_method_defined?(:dm_after_private)

define_method(:dm_then_private) { 3 }
private :dm_then_private
p Object.public_method_defined?(:dm_then_private)
p Object.private_method_defined?(:dm_then_private)

def dm_def = 4
p Object.public_method_defined?(:dm_def)
p Object.private_method_defined?(:dm_def)
