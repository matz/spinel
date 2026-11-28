# A program whose class answers `include?` itself keeps the guard: the
# program's include? is what `include M unless include?(M)` asks, and its
# answer decides. (Where no program method could answer, the guard is the
# include: test/include_guarded_by_include_p.rb. An include a run-time answer
# lets through is not one Spinel can lay out, so none here runs.)
module Callbacks
  def run_callbacks(kind)
    yield
  end
end

class Picky
  def self.include?(mod) = true   # claims every module: the include is skipped
  include Callbacks unless include?(Callbacks)
end
p Picky.new.respond_to?(:run_callbacks)
p Picky.include?(Callbacks)

