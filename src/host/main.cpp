#include <iostream>
#include <string>

#include "newpipe/auth_store.hpp"
#include "newpipe/log.hpp"
#include "newpipe/settings_store.hpp"
#include "newpipe/sponsorblock.hpp"
#include "newpipe/subtitles.hpp"
#include "newpipe/youtube_catalog_service.hpp"
#include "newpipe/youtube_resolver.hpp"

int main(int argc, char* argv[]) {
    newpipe::init_log();
    newpipe::YouTubeCatalogService service;
    std::string auth_file;
    std::string search_query;
    std::string resolve_url;
    std::string related_url;
    std::string channel_url;
    std::string playlist_url;
    std::string comments_url;
    std::string quality_arg;
    bool subscriptions_mode = false;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--quality" && i + 1 < argc) {
            quality_arg = argv[++i];
        } else if (arg == "--auth-file" && i + 1 < argc) {
            auth_file = argv[++i];
        } else if (arg == "--subscriptions") {
            subscriptions_mode = true;
        } else if (arg == "--search" && i + 1 < argc) {
            search_query = argv[++i];
        } else if (arg == "--resolve" && i + 1 < argc) {
            resolve_url = argv[++i];
        } else if (arg == "--related" && i + 1 < argc) {
            related_url = argv[++i];
        } else if (arg == "--channel" && i + 1 < argc) {
            channel_url = argv[++i];
        } else if (arg == "--playlist" && i + 1 < argc) {
            playlist_url = argv[++i];
        } else if (arg == "--comments" && i + 1 < argc) {
            comments_url = argv[++i];
        } else if (arg == "--sponsor" && i + 1 < argc) {
            // SponsorBlock segments of a video id, as the player fetches them.
            newpipe::HttpsHttpClient client;
            const auto segments = newpipe::fetch_sponsor_segments(client, argv[++i]);
            std::cout << "sponsor segments: " << segments.size() << '\n';
            for (const auto& segment : segments) {
                std::cout << "- " << segment.category << " " << segment.start << " - " << segment.end << '\n';
            }
            return 0;
        } else if (arg == "--chapters" && i + 1 < argc) {
            // Chapters of a video id, as the player fetches them.
            const auto chapters = service.get_chapters(argv[++i]);
            std::cout << "chapters: " << chapters.size() << '\n';
            for (const auto& chapter : chapters) {
                std::cout << "- " << chapter.start << " " << chapter.title << '\n';
            }
            return 0;
        }
    }

    if (!service.is_loaded()) {
        std::cerr << "service init failed: " << service.error_message() << '\n';
        return 1;
    }

    if (subscriptions_mode) {
        std::string error;
        if (!auth_file.empty()) {
            if (!service.import_auth_session_from_file(auth_file, &error)) {
                std::cerr << "auth import failed: " << error << '\n';
                return 1;
            }
        } else {
            service.load_auth_session(&error);
        }

        const auto feed = service.get_subscriptions_feed();
        if (!feed.has_value()) {
            std::cerr << "subscriptions failed: " << service.error_message() << '\n';
            return 1;
        }

        std::cout << "subscriptions: " << feed->items.size() << '\n';
        for (const auto& item : feed->items) {
            std::cout << "- " << item.title << " | " << item.channel_name << '\n';
        }
        return 0;
    }

    if (!search_query.empty()) {
        const auto results = service.search(search_query);
        std::cout << "search: " << results.query << '\n';
        for (const auto& item : results.items) {
            std::cout << "- " << item.title << " | " << item.channel_name << " | " << item.url << '\n';
        }
        return 0;
    }

    if (!related_url.empty()) {
        newpipe::StreamItem item;
        item.url = related_url;
        const auto feed = service.get_related_feed(item);
        if (!feed.has_value()) {
            std::cerr << "related failed: " << service.error_message() << '\n';
            return 1;
        }

        std::cout << "related: " << feed->items.size() << '\n';
        for (const auto& entry : feed->items) {
            std::cout << "- " << entry.title << " | " << entry.channel_name << " | " << entry.view_count_text
                      << " | " << entry.published_text << '\n';
        }
        return 0;
    }

    if (!channel_url.empty()) {
        newpipe::StreamItem item;
        item.url = channel_url;
        const auto detail = service.get_stream_detail(channel_url);
        if (detail.has_value()) {
            item = detail->item;
        }

        const auto feed = service.get_channel_feed(item);
        if (!feed.has_value()) {
            std::cerr << "channel failed: " << service.error_message() << '\n';
            return 1;
        }

        std::cout << "channel: " << feed->kiosk.title << " (" << feed->items.size() << ")\n";
        for (const auto& entry : feed->items) {
            std::cout << "- " << entry.title << " | " << entry.published_text << '\n';
        }
        return 0;
    }

    if (!playlist_url.empty()) {
        newpipe::StreamItem item;
        item.url = playlist_url;
        const auto feed = service.get_playlist_feed(item);
        if (!feed.has_value()) {
            std::cerr << "playlist failed: " << service.error_message() << '\n';
            return 1;
        }

        std::cout << "playlist: " << feed->kiosk.title << " (" << feed->items.size() << ")\n";
        for (const auto& entry : feed->items) {
            std::cout << "- " << entry.title << " | " << entry.channel_name << '\n';
        }
        return 0;
    }

    if (!comments_url.empty()) {
        newpipe::StreamItem item;
        item.url = comments_url;
        const auto page = service.get_comments(item);
        if (!page.has_value()) {
            std::cerr << "comments failed: " << service.error_message() << '\n';
            return 1;
        }

        std::cout << "comments: " << page->title << " count=" << page->count_text << " (" << page->items.size()
                  << ")\n";
        for (const auto& sort : page->sorts) {
            std::cout << "sort: " << sort.title << (sort.selected ? " (selected)" : "") << '\n';
        }
        for (const auto& entry : page->items) {
            std::cout << "- " << entry.author_name << " | likes=" << entry.like_count_text
                      << " replies=" << entry.reply_count_text << (entry.replies_token.empty() ? "" : " +token")
                      << (entry.pinned_text.empty() ? "" : " [" + entry.pinned_text + "]") << " | " << entry.body
                      << '\n';
        }
        // The first comment with replies: its first page of them.
        for (const auto& entry : page->items) {
            if (entry.replies_token.empty()) {
                continue;
            }
            const auto replies = service.get_comments_page(entry.replies_token);
            std::cout << "replies of " << entry.author_name << ": "
                      << (replies ? std::to_string(replies->items.size()) : std::string("failed"))
                      << (replies && !replies->next_page_token.empty() ? " more" : "") << '\n';
            if (replies && !replies->items.empty()) {
                std::cout << "  first reply: " << replies->items.front().author_name << " | "
                          << replies->items.front().body << '\n';
            }
            break;
        }
        // The other order's first page.
        for (const auto& sort : page->sorts) {
            if (sort.selected) {
                continue;
            }
            const auto sorted = service.get_comments_page(sort.token);
            std::cout << "sorted by " << sort.title << ": "
                      << (sorted ? std::to_string(sorted->items.size()) : std::string("failed"))
                      << " first=" << (sorted && !sorted->items.empty() ? sorted->items.front().author_name : "")
                      << '\n';
        }
        // Two more pages through the continuation token, as the comment screen does.
        std::string token = page->next_page_token;
        for (int n = 2; n <= 3 && !token.empty(); n++) {
            const auto next = service.get_comments_page(token);
            if (!next.has_value()) {
                std::cerr << "comments page " << n << " failed: " << service.error_message() << '\n';
                return 1;
            }
            std::cout << "comments page " << n << ": " << next->items.size() << " first="
                      << (next->items.empty() ? "" : next->items.front().author_name) << '\n';
            token = next->next_page_token;
        }
        return 0;
    }

    if (!resolve_url.empty()) {
        if (!quality_arg.empty()) {
            newpipe::PlaybackQualityMode mode = newpipe::PlaybackQualityMode::BEST;
            if (quality_arg == "1080") {
                mode = newpipe::PlaybackQualityMode::HD_1080;
            } else if (quality_arg == "720") {
                mode = newpipe::PlaybackQualityMode::HD_720;
            } else if (quality_arg == "320" || quality_arg == "360") {
                mode = newpipe::PlaybackQualityMode::LOW_320;
            }
            newpipe::SettingsStore::instance().update_playback_quality(mode);
        }
        newpipe::YouTubeResolver resolver;
        std::string error;
        const auto resolved = resolver.resolve(
            resolve_url,
            error,
            [](const std::string& title, const std::string& detail) {
                std::cout << title << " :: " << detail << '\n';
            });
        if (!resolved.has_value()) {
            std::cerr << "resolve failed: " << error << '\n';
            return 1;
        }

        std::cout << "stream: " << resolved->stream_url << '\n';
        if (!resolved->external_audio_url.empty()) {
            std::cout << "audio: " << resolved->external_audio_url << '\n';
        }
        std::cout << "referer: " << resolved->referer << '\n';
        std::cout << "quality: " << resolved->quality_label << '\n';
        std::cout << "live: " << (resolved->is_live ? "yes" : "no") << '\n';
        // --captions <language> after --resolve: the track the player would show, first cues.
        for (int k = 1; k + 1 < argc; k++) {
            if (std::string(argv[k]) == "--captions") {
                const auto choice = newpipe::pick_subtitle_track(resolved->captions, argv[k + 1], resolved->audio_language);
                std::cout << "audio language: " << resolved->audio_language << '\n';
                if (!choice) {
                    std::cout << "no subtitle track\n";
                    return 0;
                }
                std::cout << "subtitle track: " << choice->language_code << (choice->auto_generated ? " (auto)" : "") << '\n';
                std::cout << "subtitle url: " << choice->base_url << '\n';
                newpipe::HttpsHttpClient client;
                const auto cues = newpipe::fetch_subtitle_cues(client, *choice);
                std::cout << "cues: " << cues.size() << '\n';
                for (size_t c = 0; c < cues.size() && c < 6; c++) {
                    std::cout << cues[c].start << "-" << cues[c].end << " | " << cues[c].text << '\n';
                }
                return 0;
            }
        }
        for (const auto& track : resolved->captions) {
            std::cout << "caption: " << track.language_code << (track.auto_generated ? " (auto)" : "")
                      << (track.translatable ? " translatable" : "") << " | " << track.name << '\n';
        }
        return 0;
    }

    const auto kiosks = service.list_kiosks();
    std::cout << "kiosks: " << kiosks.size() << '\n';
    for (const auto& kiosk : kiosks) {
        const auto feed = service.get_home_feed(kiosk.id);
        std::cout << "* " << kiosk.id;
        if (feed.has_value()) {
            std::cout << " (" << feed->items.size() << " items)";
        }
        std::cout << '\n';
    }

    if (!kiosks.empty()) {
        const auto feed = service.get_home_feed(kiosks.front().id);
        if (feed.has_value() && !feed->items.empty()) {
            const auto detail = service.get_stream_detail(feed->items.front().url);
            if (detail.has_value()) {
                std::cout << "sample: " << detail->item.title << '\n';
            }
        }
    }

    return 0;
}
