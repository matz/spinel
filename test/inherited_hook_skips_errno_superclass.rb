# a class under an Errno class, written as a path, with a program `inherited`
# hook extended onto another class: the core's hook fires nothing (Rack's
# `class MultipartPartLimitError < Errno::EMFILE`)
module Tracker
  def inherited(base)
    super
    (@subs ||= []) << base.name
  end
  def subs = @subs
end

class Base
  extend Tracker
end
class Sub < Base; end

module BadRequest; end
class PartLimitError < Errno::EMFILE
  include BadRequest
end

p Base.subs
p PartLimitError.ancestors.include?(Errno::EMFILE), PartLimitError.new.is_a?(SystemCallError)
p PartLimitError.new.message
