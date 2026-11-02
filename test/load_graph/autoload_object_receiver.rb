# Object.autoload registers the constant on Object, as the bare autoload at
# the top level does: from the top level, from a module body, through an
# alias of Object and through ::Object. The file loads at the constant's
# first reference.
O = Object
module M
  Object.autoload(:FromM, File.join(__dir__, "autoload_object_receiver_m_lib"))
  puts "in M"
end
O.autoload(:ViaAlias, File.join(__dir__, "autoload_object_receiver_alias_lib"))
::Object.autoload(:ViaRoot, File.join(__dir__, "autoload_object_receiver_root_lib"))
puts "registered"
p FromM
p ViaAlias
p ViaRoot
