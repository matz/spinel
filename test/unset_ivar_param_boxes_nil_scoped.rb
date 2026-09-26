# The same through a scoped receiver: Views::Inbox.label(@page) (#5091).
class Ctl2
  def set_page
    @page = 2
  end

  def all
    set_page
    Views::Inbox.label(@page)
  end

  def unread
    Views::Inbox.label(@page)
  end
end

module Views
  module Inbox
    def self.label(page)
      io = +""
      label_into(io, page)
      io
    end

    def self.label_into(io, page)
      io << (page && page > 1 ? "prev" : "first")
    end
  end
end

puts Ctl2.new.all
puts Ctl2.new.unread
puts Views::Inbox.label_into(+"", 2.5)
