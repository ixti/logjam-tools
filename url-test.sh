#!/bin/bash

# Happy hacking stefan.kaes!

# failures
curl -v http://127.0.0.1:9678/
curl -v http://127.0.0.1:9678/foo/
curl -v http://127.0.0.1:9678/tools/foo/
curl -v http://127.0.0.1:9678/logjam/
curl -v http://127.0.0.1:9678/tools/barf/

# successes

alive_url='http://127.0.0.1:9678/alive.txt'

ajax_url='http://127.0.0.1:9678/logjam/ajax?logjam_caller_id=profiles-production-3fef40d26fc211e4bf2c005056bc1b5f&logjam_caller_action=Profiles::ProfilesController%23show&logjam_request_id=perlapp-production-f0ca1b7a6fc211e4a03854af99b42fc2&logjam_action=AddressBook%23contact_lightbox.save&rts=1416384376313,1416384377630&url=/app/contact&v=1'

page_url='http://127.0.0.1:9678/logjam/page?logjam_action=Contacts::RecommendationsController%23index&logjam_request_id=contacts-production-be9e5cc06fcb11e49b580050569ea111&url=/contacts/recommendations&viewport_height=955&viewport_width=1920&redirect_count=0&v=1&html_nodes=352&script_nodes=13&style_nodes=2&rts=1416388135643,1416388135646,1416388135646,1416388135646,1416388135646,1416388135646,1416388135972,1416388135973,1416388136115,1416388135973,1416388136164,1416388136164,1416388136181,1416388136184,1416388136185,1416388136188'

curl -v $alive_url
curl -v $ajax_url
curl -v $page_url

# siege -q -c 100 $url
