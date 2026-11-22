# included_hook_aliases.rb with a subclass of Hash among the includers: an
# alias there may be one of a builtin's method, which the step ahead of the
# walk reads.
module AhNames
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
class AhPerson
  attr_reader :first, :last
  def initialize(f, l) = (@first = f; @last = l)
  def self.make(f, l) = new(f, l)
  include AhNames
end
class AhPet < Hash
  def first = "Rex"
  def last = "Dog"
  def self.make = new
  include AhNames
  def full = "pet #{first}"
end
pa = AhPerson.build("Ada", "Lovelace")
p pa.name, pa.to_label, pa.title
AhPerson.mode = :strict
p AhPerson.mode, AhPet.mode
pe = AhPet.build
p pe.full, pe.name, pe.title
