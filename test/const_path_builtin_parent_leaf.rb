# With a program class T::Stat, `File::Stat == T::Stat` is false, as in
# CRuby. Enumerator::Yielder, Random::Formatter and File::Constants beside
# T::Yielder, T::Formatter and T::Constants give the same result. A
# reopened builtin (class Encoding::Converter) written after T::Converter
# returns its class method.
module T
  class Stat; end
  class Converter; end
  class Yielder; end
  class Formatter; end
  module Constants; end
end

class Encoding::Converter
  def self.tag
    "conv"
  end
end

stat = begin
  File::Stat
rescue NameError
  nil
end
p stat == T::Stat

yielder = begin
  Enumerator::Yielder
rescue NameError
  nil
end
p yielder == T::Yielder

formatter = begin
  Random::Formatter
rescue NameError
  nil
end
p formatter == T::Formatter

constants = begin
  File::Constants
rescue NameError
  nil
end
p constants == T::Constants

p Encoding::Converter.tag
p T::Converter
