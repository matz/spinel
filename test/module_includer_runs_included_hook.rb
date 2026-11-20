# A module that includes a module with a `self.included(base)` hook gets what
# the hook sets up, as a class does: here `base.extend(ClassMethods)`, so the
# module answers the class methods to its own body, to a method elsewhere
# and to the top level. The same for an include with the module as receiver,
# and for a class, which worked before.
module Initializable
  def self.included(base) = base.extend(ClassMethods)
  module ClassMethods
    def initializers = (@initializers ||= [])
    def initializer(name, &blk) = initializers << [name, blk]
  end
end

module Bootstrap
  include Initializable
  initializer(:load_config) { :config }
  p initializers.size
end

module Finisher; end
Finisher.include(Initializable)
Finisher.initializer(:eager_load) { :eager }

class Railtie
  include Initializable
  initializer(:set_paths) { :paths }
end

class Application
  def initializers = Bootstrap.initializers + Railtie.initializers + Finisher.initializers
end

Application.new.initializers.each { |name, blk| p [name, blk.call] }
p Bootstrap.initializers.size, Finisher.initializers.size
