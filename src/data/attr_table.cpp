#include "data/attr_table.h"
#include "data/gdal_common.h"
#include "data/geom_util.h"
#include "util/logger.h"
#include <gdal.h>
#include <ogr_api.h>
#include <cstring>
#include <cstdio>

namespace {

// 依据字段定义(来自图层)构造一整行 AttrCell
void buildCells(OGRFeatureH f, const AttrLayerInfo& info, TextEncoding enc, AttrRow& r) {
    int nf = (int)info.fields.size();
    r.cells.reserve(nf);
    for (int k = 0; k < nf; k++) {
        const AttrFieldDef& fd = info.fields[k];
        AttrCell c;
        c.rawName = fd.rawName;
        c.name = decodeRawToUtf8(fd.rawName, enc);
        c.isString = fd.isString;
        if (OGR_F_IsFieldSetAndNotNull(f, k)) {
            OGRFieldType t = OFTString;
            OGRFieldDefnH fdef = OGR_F_GetFieldDefnRef(f, k);
            if (fdef) t = OGR_Fld_GetType(fdef);
            char buf[64];
            switch (t) {
                case OFTInteger:
                    snprintf(buf, sizeof(buf), "%d", OGR_F_GetFieldAsInteger(f, k)); c.text = buf; break;
                case OFTInteger64:
                    snprintf(buf, sizeof(buf), "%lld", (long long)OGR_F_GetFieldAsInteger64(f, k)); c.text = buf; break;
                case OFTReal: case OFTRealList:
                    snprintf(buf, sizeof(buf), "%.10g", OGR_F_GetFieldAsDouble(f, k)); c.text = buf; break;
                case OFTDate: case OFTDateTime: case OFTTime: {
                    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, tz = 0;
                    if (OGR_F_GetFieldAsDateTime(f, k, &y, &mo, &d, &h, &mi, &s, &tz))
                        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
                    else c.text = "NULL";
                    break;
                }
                default: {
                    // 字符串字段: 保留原始字节, 按 enc 转码显示
                    const char* s = OGR_F_GetFieldAsString(f, k);
                    if (s) {
                        const char* e = s;
                        while (*e) e++;
                        c.raw.assign((const unsigned char*)s, (const unsigned char*)e);
                        c.text = decodeRawToUtf8(c.raw, enc);
                    }
                }
            }
        } else {
            c.isNull = true;
            c.text = "NULL";
        }
        r.cells.push_back(std::move(c));
    }
}

AttrRow buildRow(OGRFeatureH f, const AttrLayerInfo& info, TextEncoding enc) {
    AttrRow r;
    r.fid = OGR_F_GetFID(f);
    buildCells(f, info, enc, r);
    OGRGeometryH g = OGR_F_GetGeometryRef(f);
    if (g) {
        addFilledGeometry(g, r.outline, r.fillTris, &r.points);
        r.hasGeom = !(r.outline.empty() && r.points.empty() && r.fillTris.empty());
    }
    return r;
}

}  // namespace

bool attrOpenLayer(const std::string& path, int layerIdx, AttrLayerInfo& info) {
    info = AttrLayerInfo{};
    auto kDS = gdalKeeperEnsure(path);
    if (!kDS) return false;
    std::unique_lock<std::mutex> ul(kDS->mu);
    if (!gdalKeeperWait(kDS, ul)) return false;
    OGRLayerH lyr = GDALDatasetGetLayer(kDS->ds, layerIdx);
    if (!lyr) return false;

    OGRDataSourceH ds = kDS->ds;
    OGRSFDriverH drv = ds ? OGR_DS_GetDriver(ds) : nullptr;
    const char* dn = drv ? OGR_Dr_GetName(drv) : nullptr;
    info.driverName = dn ? dn : "";
    info.canEncode = (info.driverName == "ESRI Shapefile");

    const char* ln = OGR_L_GetName(lyr);
    info.layerName = ln ? ln : "";
    info.total = -1;   // 总数不在此取(可能慢), 由 attrFeatureCount 异步补全

    OGRFeatureDefnH def = OGR_L_GetLayerDefn(lyr);
    if (def) {
        int nf = OGR_FD_GetFieldCount(def);
        info.fields.reserve(nf);
        for (int k = 0; k < nf; k++) {
            OGRFieldDefnH fdef = OGR_FD_GetFieldDefn(def, k);
            AttrFieldDef fd;
            if (fdef) {
                const char* nm = OGR_Fld_GetNameRef(fdef);
                if (nm) {
                    const char* e = nm;
                    while (*e) e++;
                    fd.rawName.assign((const unsigned char*)nm, (const unsigned char*)e);
                }
                fd.width = OGR_Fld_GetWidth(fdef);
                fd.precision = OGR_Fld_GetPrecision(fdef);
                OGRFieldType t = OGR_Fld_GetType(fdef);
                fd.isString = (t == OFTString || t == OFTStringList);
            }
            info.fields.push_back(std::move(fd));
        }
    }
    info.ok = true;
    return true;
}

long long attrFeatureCount(const std::string& path, int layerIdx) {
    auto kDS = gdalKeeperEnsure(path);
    if (!kDS) return -1;
    std::unique_lock<std::mutex> ul(kDS->mu);
    if (!gdalKeeperWait(kDS, ul)) return -1;
    OGRLayerH lyr = GDALDatasetGetLayer(kDS->ds, layerIdx);
    if (!lyr) return -1;
    long long n = OGR_L_GetFeatureCount(lyr, FALSE);
    return n < 0 ? -1 : n;
}

bool attrFetchPage(const std::string& path, int layerIdx, int page, int rowsPerPage,
                   TextEncoding enc, const AttrLayerInfo& info, AttrPageData& out) {
    out = AttrPageData{};
    out.page = page;
    if (!info.ok || rowsPerPage <= 0 || page < 0) return false;

    auto kDS = gdalKeeperEnsure(path);
    if (!kDS) return false;
    std::unique_lock<std::mutex> ul(kDS->mu);
    if (!gdalKeeperWait(kDS, ul)) return false;
    OGRLayerH lyr = GDALDatasetGetLayer(kDS->ds, layerIdx);
    if (!lyr) return false;

    long long start = (long long)page * rowsPerPage;
    bool rnd = OGR_L_TestCapability(lyr, "RandomRead") != FALSE;

    if (rnd) {
        OGR_L_ResetReading(lyr);
        for (int i = 0; i < rowsPerPage; i++) {
            OGRFeatureH f = OGR_L_GetFeature(lyr, start + i);
            if (!f) continue;   // FID 空洞/缺失: 该行跳过
            out.rows.push_back(buildRow(f, info, enc));
            OGR_F_Destroy(f);
        }
    } else {
        // 无随机读: 顺序跳过 start 条后再收集 rowsPerPage 条
        OGR_L_ResetReading(lyr);
        long long idx = 0;
        OGRFeatureH f;
        while ((f = OGR_L_GetNextFeature(lyr)) != nullptr) {
            if (idx >= start && (long long)out.rows.size() < rowsPerPage) {
                out.rows.push_back(buildRow(f, info, enc));
            }
            idx++;
            OGR_F_Destroy(f);
            if ((long long)out.rows.size() >= rowsPerPage) break;
        }
    }

    if (!out.rows.empty()) out.firstFid = out.rows[0].fid;
    return true;
}

void attrReencodePage(AttrPageData& p, TextEncoding enc) {
    for (auto& r : p.rows)
        for (auto& c : r.cells) {
            c.name = decodeRawToUtf8(c.rawName, enc);
            if (c.isString) c.text = decodeRawToUtf8(c.raw, enc);
        }
}
