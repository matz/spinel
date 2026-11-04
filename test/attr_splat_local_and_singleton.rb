# attr_accessor(*list) where list is a local of the body, written once ahead
# of the call to an Array literal of Symbols or Strings, as a constant already
# is; and the splat through singleton_class, with a local or a constant.
class Widget
  fields = [:width, :height]
  attr_accessor(*fields)
  flags = %w[visible locked]
  attr_reader(*flags)
  def initialize
    @width = 3; @height = 4; @visible = true; @locked = false
  end
end
w = Widget.new
w.width = 10
p w.width, w.height, w.visible, w.locked

module Settings
  settings = [:image_loading, :image_decoding]
  singleton_class.attr_accessor(*settings)
  NAMES = [:preload, :nonce].freeze
  singleton_class.attr_accessor(*NAMES)
  self.image_loading = "lazy"
  self.preload = true
end
Settings.image_decoding = "async"
p Settings.image_loading, Settings.image_decoding, Settings.preload, Settings.nonce

class Panel
  include Settings
  shown = [:title]
  singleton_class.attr_writer(*shown)
  def self.title = @title
end
Panel.title = "Panel"
p Panel.title
