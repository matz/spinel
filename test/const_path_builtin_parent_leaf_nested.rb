# Inside the program's module App, File::Stat resolves to App::File::Stat.
# A reopened builtin (class Encoding::Converter) written before
# T::Converter resolves to Encoding::Converter.
class Encoding::Converter
  def self.tag
    "conv"
  end
end

module T
  class Converter; end
end

module App
  module File
    class Stat
      def kind
        "app stat"
      end
    end
  end

  def self.stat
    File::Stat.new.kind
  end

  def self.stat_class
    File::Stat
  end
end

p Encoding::Converter
p Encoding::Converter.tag
p T::Converter
p T::Converter.respond_to?(:tag)
p App.stat
p App.stat_class
