module Esc
  def escape(s) = s.gsub("<", "&lt;")
  alias e escape
  module_function :e, :escape
end
