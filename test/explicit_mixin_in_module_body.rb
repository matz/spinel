# `K.extend(M)`, `K.include(M)` or `K.prepend(M)` as a statement of a module
# or class body, where that module itself declares K (in this body or another
# of its reopenings): the reopening `class K; extend M; end` nested there is
# the same K, as the constant resolves to it first. Before, only the body's
# own class, or a statement of the program's top level, was rewritten, and
# the call was refused as Object#extend.
module Rails
  module HTML
    class Sanitizer
      def self.base = "base"
    end
    module Concern
      def shared = "shared"
    end
  end
  module HTML4
    class Sanitizer < Rails::HTML::Sanitizer
      module VendorMethods
        def full_sanitizer = "full4"
        def safe_list_sanitizer = "safe4"
      end
      extend VendorMethods
    end
    class FullSanitizer < Rails::HTML::Sanitizer; end
  end
  module HTML
    Sanitizer.extend(HTML4::Sanitizer::VendorMethods) # :nodoc:
    Sanitizer.include(Concern)
    module Vendor
      def vendor = "v"
    end
    Concern.extend(Vendor)
  end
end
p Rails::HTML::Sanitizer.full_sanitizer, Rails::HTML4::FullSanitizer.safe_list_sanitizer, Rails::HTML::Sanitizer.base
p Rails::HTML::Sanitizer.new.shared, Rails::HTML4::FullSanitizer.new.shared, Rails::HTML::Concern.vendor
p Rails::HTML4::Sanitizer.full_sanitizer
