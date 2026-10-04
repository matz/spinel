# Kernel.require_relative loads a file whose name the program computes at
# run time. The file defines Gadget, so in CRuby Widget is a subclass of
# Gadget. Spinel cannot read that file, so it refuses the class instead of
# rescuing to Base.
Kernel.require_relative File.join("class_paren_superclass_kernel_require", "gadget")

class Base
end

class Widget < (Gadget rescue Base)
end

puts Widget.superclass
