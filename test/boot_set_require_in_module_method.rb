# A `require "set"` in a module's method and one used as a condition's
# value, with Set also reached by `to_set`.
module M
  def self.go
    require "set"
  end
end
p M.go
p(require("set") ? 1 : 2)
p [1, 2].to_set.size
