# A library spinel provides from Ruby source (packages/erb) is a file of its
# own under the graph load model. defined?(ERB::Escape) after its require
# asks the library's own class, whose file runs ahead of the guard, so the
# guard decides at compile time as it does under the inline model. The
# define_method arm is the only definition of escape_html when its guard
# holds (the module has no def of the name), and a def arm answers the same
# when the guard names a constant the source does not define (ERB::NoSuch).
require "erb"

module Utils
  if defined?(ERB::Escape) && ERB::Escape.instance_method(:html_escape)
    define_method(:escape_html, ERB::Escape.instance_method(:html_escape))
  end
  module_function :escape_html
end

module Other
  if defined?(ERB::NoSuch)
    define_method(:escape_html, ERB::Escape.instance_method(:html_escape))
  else
    def escape_html(s) = "else:" + ERB::Util.html_escape(s)
  end
  module_function :escape_html
end

class Plain
  if defined?(ERB::Escape)
    def which = "then"
  else
    def which = "else"
  end
end

if defined?(ERB::Escape)
  def top_which = "then"
else
  def top_which = "else"
end

p Utils.escape_html("a&<b>\"c'")
p Other.escape_html("a<b")
p Plain.new.which
p top_which
