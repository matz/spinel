# `super` in a class method named subclasses lists the subclasses whose
# header has run, as Class#subclasses on a constant does: a class is a
# subclass from the moment its header runs. Rails' Railtie filters
# Class#subclasses that way, and DescendantsTracker walks it from class
# bodies (define_callbacks in Engine's body) before any Application exists.
module Tracker
  @excluded = { :never => true }
  class << self
    def reject!(classes)
      if @excluded
        classes.reject! { |d| puts "excluded?(#{d.name})"; @excluded.key?(d) }
      end
      classes
    end
  end
  def descendants
    subclasses = Tracker.reject!(self.subclasses)
    subclasses.concat(subclasses.flat_map(&:descendants))
  end
end

class Railtie
  ABSTRACT = %w(Railtie Engine Application).freeze
  extend Tracker
  class << self
    def subclasses
      super.reject { |k| ABSTRACT.include?(k.name) }.sort_by(&:name)
    end
  end
end

class Engine < Railtie
  p(([self] + descendants).map(&:name))
end
class Application < Engine
  p(([self] + descendants).map(&:name))
  p Engine.subclasses, Railtie.subclasses
end
class I18nRailtie < Railtie; end
class MyApp < Application
  p superclass.subclasses.map(&:name)
end
class Lazy < Application; end

p Engine.descendants.map(&:name)
p Application.subclasses.map(&:name)
p Railtie.descendants.map(&:name)
