/*
 * Copyright (C) 2013 Canonical, Ltd.
 *
 * Authors:
 *    Jussi Pakkanen <jussi.pakkanen@canonical.com>
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of version 3 of the GNU General Public License as published
 * by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "MediaFile.hh"
#include "internal/utils.hh"
#include "MetadataExtractor.hh"

#include <glib-object.h>
#include <gio/gio.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <stdexcept>
#include <memory>
#include <libgen.h>
#include <iostream>
#include <iomanip>
#include <algorithm>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <taglib/taglib.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

using namespace std;

namespace mediascanner
{

MetadataExtractor::MetadataExtractor(int seconds)
{
}

MetadataExtractor::~MetadataExtractor()
{
}

DetectedFile MetadataExtractor::detect(const std::string &path)
{
    std::unique_ptr<GFile, void(*)(void *)> file(
        g_file_new_for_path(path.c_str()), g_object_unref);
    if (!file) {
        throw runtime_error("Could not create file object");
    }

    GError *error = nullptr;
    std::unique_ptr<GFileInfo, void(*)(void *)> info(
        g_file_query_info(
            file.get(),
            G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE ","
            G_FILE_ATTRIBUTE_ETAG_VALUE,
            G_FILE_QUERY_INFO_NONE, /* cancellable */ NULL, &error),
        g_object_unref);
    if (!info) {
        string errortxt(error->message);
        g_error_free(error);

        string msg("Query of file info for ");
        msg += path;
        msg += " failed: ";
        msg += errortxt;
        throw runtime_error(msg);
    }

    string etag(g_file_info_get_etag(info.get()));
    string content_type(g_file_info_get_attribute_string(
        info.get(), G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE));
    if (content_type.empty()) {
        throw runtime_error("Could not determine content type.");
    }

    MediaType type;
    if (content_type.find("audio/") == 0) {
        type = AudioMedia;
    } else if (content_type.find("video/") == 0) {
        type = VideoMedia;
    } else if (content_type.find("image/") == 0) {
        type = ImageMedia;
    } else {
        type = MiscMedia;
    }

    return DetectedFile(path, etag, content_type, type);
}

const char *get_filename_extension(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if(!dot || dot == filename) return "";
    return dot + 1;
}

void MetadataExtractor::extractForAudio(MediaFile &mf, const DetectedFile &d)
{
    TagLib::FileRef file(d.path.c_str());

    if (!file.isNull()) {

        TagLib::PropertyMap tags = file.file()->properties();

        if (tags.contains("ALBUM"))
            mf.setAlbum(file.tag()->album().toCString());
        if (tags.contains("ARTIST"))
            mf.setArtist(file.tag()->artist().toCString());
        if (tags.contains("TITLE"))
            mf.setTitle(file.tag()->title().toCString());
        if (tags.contains("GENRE"))
            mf.setGenre(file.tag()->genre().toCString());
        mf.setTrackPosition(file.tag()->track());
        mf.setYear(file.tag()->year());

        if (tags.contains("ALBUMARTIST")) {
            TagLib::StringList values = tags["ALBUMARTIST"];
             mf.setAlbumArtist(values.toString(", ").toCString());
        }
        else {
            mf.setAlbumArtist(mf.artist());
        }

        if (tags.contains("TRACKNUMBER")) {
            TagLib::StringList values = tags["TRACKNUMBER"];
            TagLib::StringList trackNumberParts = values.toString().split("/");
            if (trackNumberParts.size() == 2) {
                mf.setTrackPosition(trackNumberParts[0].toInt());
                mf.setTrackTotal(trackNumberParts[1].toInt());
            }
        }

        if (tags.contains("DISCNUMBER")) {
            TagLib::StringList values = tags["DISCNUMBER"];
            TagLib::StringList discNumberParts = values.toString().split("/");
            if (discNumberParts.size() == 2) {
                mf.setDiscTotal(discNumberParts[1].toInt());
            }
        }

        // FIXME parse for more tags. See http://taglib.github.io/api/classTagLib_1_1PropertyMap.html#a8b0c96a6df5b64a36654dc843b2375bc
        // for a list of possible available tags
    }
}

std::string MetadataExtractor::getAlbumPathFromImage(const std::string& path)
{
    std::string pathInLower = path;
    std::transform(pathInLower.begin(), pathInLower.end(), pathInLower.begin(), ::tolower);

    std::string dcimPath = "/media/internal/dcim";
    if (pathInLower.substr(0, dcimPath.size()) == dcimPath)
        return std::string("camera://");

    return path.substr(0, path.find_last_of("/"));
}

std::string MetadataExtractor::getAlbumNameFromPath(const std::string& path)
{
    return path.substr(path.find_last_of("/") + 1, path.size() - 1);
}

void MetadataExtractor::extractForImage(MediaFile &mf, const DetectedFile &d)
{
    mf.setMediaType("image");
    mf.setAlbumPath(getAlbumPathFromImage(mf.path()));
    mf.setAlbum(getAlbumNameFromPath(mf.albumPath()));

    // FIXME once we have exif detection override createdTime here with the correct value
    // mf.setCreatedTime(0);
}

MediaFile MetadataExtractor::extract(const DetectedFile &d)
{
    MediaFile mf;
    mf.setPath(d.path);
    mf.setEtag(d.etag);
    mf.setType(d.type);

    std::string fileNameWithExtension = basename(const_cast<char *>(d.path.c_str()));
    std::string pureFileName = fileNameWithExtension.substr(0, fileNameWithExtension.find_last_of("."));

    mf.setName(pureFileName);
    mf.setExtension(get_filename_extension(d.path.c_str()));

    // FIXME better take the creation time of the file itself and not when we have discovered the file
    mf.setCreatedTime(time(NULL));

    struct stat st;
    if (stat(d.path.c_str(), &st) == 0) {
        mf.setModifiedTime(st.st_mtime);
        mf.setSize(st.st_size);
    }

    if (d.type == AudioMedia)
        extractForAudio(mf, d);
    else if (d.type == ImageMedia)
        extractForImage(mf, d);

    mf.rebuildSearchKey();

    return mf;
}

}
