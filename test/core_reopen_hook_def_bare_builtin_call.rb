# A hook gives String, a core class reopened, a def whose bare call is a
# builtin String method (`split`), through `base.class_eval`. The walk
# fires the hook in the reopening; the step that writes such a call on
# self did not read the body the walk splices, and the call was refused.
# It is a lowering row too now, which the walk's splice runs.
module Wordy
  def self.included(base)
    base.class_eval do
      def words = split(" ")
      alias_method :w, :words
    end
  end
  module Maker
    def blank = new("")
  end
end
class String
  include Wordy
end
p "a b c".words, "x y".w
