class Article
  module Executable
    def title_given?
      !title_text.nil?
    end

    def via_send
      public_send("title_text")
    end
  end
end
