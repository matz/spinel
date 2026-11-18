require_relative "computed_pattern_prag_lib"
class ComputedPatternDb
  include ComputedPatternPrag
  def enc = "enc:" + super
end
