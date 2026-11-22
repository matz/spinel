# A `super` in a class method named subclasses reaches Class#subclasses, whose
# elements are class values: a class method of the program's answers them
# through a boxed receiver (the parameter of a symbol-to-proc block), as it
# answers the elements of a constant's `subclasses`.
class Railtie
  ABSTRACT_RAILTIES = %w(Railtie Engine Application).freeze
  LOAD_ORDER = []

  class << self
    def subclasses
      super.reject(&:abstract_railtie?).sort_by(&:load_index)
    end

    def abstract_railtie?
      ABSTRACT_RAILTIES.include?(name)
    end

    def load_index = LOAD_ORDER.index(name)

    def inherited(subclass)
      LOAD_ORDER << subclass.name
      super
    end
  end
end

class Engine < Railtie; end
class Application < Engine; end
class I18nRailtie < Railtie; end
class ASRailtie < Railtie; end
class MyApp < Application; end

p Railtie.subclasses.map(&:name)
p Engine.subclasses.map(&:name)
p Application.subclasses.map(&:name)
p Railtie.subclasses.map(&:abstract_railtie?)
