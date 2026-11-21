class RegionMade
  def m = :m
  def self.k = :k
  class In; def i = :i; end
end
module RegionLoud; def hi = "LOUD " + super; end
class RegionFoo; prepend RegionLoud; end
