# Aliases in an included hook: alias_method and the alias keyword in the
# class_eval block, an alias_method on the hook's parameter, and
# singleton_class.alias_method and singleton_class.attr_accessor for the
# class side.
module AlNames
  def self.included(base)
    base.class_eval do
      def full = "#{first} #{last}"
      alias_method :name, :full
      alias to_label full
      singleton_class.alias_method :build, :make
      singleton_class.attr_accessor :mode
    end
    base.alias_method :title, :full
  end
end
class AlPerson
  attr_reader :first, :last
  def initialize(f, l) = (@first = f; @last = l)
  def self.make(f, l) = new(f, l)
  include AlNames
end
class AlPet
  def first = "Rex"
  def last = "Dog"
  def self.make = new
  include AlNames
  def full = "pet #{first}"
end
pa = AlPerson.build("Ada", "Lovelace")
p pa.name, pa.to_label, pa.title
AlPerson.mode = :strict
p AlPerson.mode, AlPet.mode
pe = AlPet.build
p pe.full, pe.name, pe.title
