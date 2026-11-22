# A module_function's name is the module's method where the module, a module
# it includes (through another's include too, a builtin one's too) or Object
# and Kernel define it, as CRuby looks it up from past the module's own
# prepends; a singleton class has no module_function, a module's
# `class << self` body's self too, at any depth, and a bare word there or in
# a class raises NameError.
module N1; def nb = :nb; end
module N2; include N1; def nc = :nc; end
module NM
  include N2
  module_function :nc
  module_function :nb
end
p [NM.nb, NM.nc]
module NK
  def self.go = (module_function(:inspect); :ok)
end
p NK.go
module NS
  def self.s = :s
  class << self
    begin
      module_function :s
    rescue NoMethodError => e
      p e.message
    end
  end
end
p NS.s
class KS
  class << self
    begin
      module_function :x
    rescue NoMethodError => e
      p e.message
    end
  end
end
module NC; include Comparable; end
module NB
  include NC
  include Enumerable
  def self.go = (module_function(:clamp, :map); :ok)
end
p NB.go
module ND
  def self.s = :s
  class << self
    class << self
      begin
        module_function :s
      rescue NoMethodError => e
        p e.message
      end
    end
  end
end
p ND.s
module NP
  prepend Comparable
  begin
    module_function :between?
  rescue NameError => e
    p e.class
  end
end
class KP
  begin
    module_function
  rescue NameError => e
    p [e.class, e.message]
  end
end
