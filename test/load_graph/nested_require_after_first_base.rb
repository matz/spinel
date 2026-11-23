require_relative "nested_require_after_first_flash"

class Base
  def go = Flash.new.tap { |f| f["a"] = 1 }
end
