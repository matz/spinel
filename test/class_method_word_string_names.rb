# A class-method visibility word in a method body whose names are Strings
# reads them as the Symbols: it sets the visibility where it runs, and one
# naming a method nothing defines raises NameError.
class Panel
  def self.show = :show
  def self.tint = :tint
  def self.quiet = private_class_method("show", "tint")
  def self.loud = public_class_method(["show"])
  def self.lost = private_class_method("gone")
end

p [Panel.show, Panel.respond_to?(:tint)]
p [Panel.quiet, (Panel.show rescue $!.class), (Panel.tint rescue $!.class), Panel.respond_to?(:show)]
p [Panel.loud, Panel.show, (Panel.tint rescue $!.class), Panel.singleton_methods.sort]
p((Panel.lost rescue [$!.class, $!.message]))
