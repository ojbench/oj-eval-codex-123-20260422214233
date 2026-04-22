#include <vector>
#include <fstream>
#include <cstddef>
#include <algorithm>

namespace sjtu {
  using fstream = std::fstream;
} // namespace sjtu

// Disk event types
enum class EventType { NORMAL, FAILED, REPLACED };

class RAID5Controller {
private:
  std::vector<sjtu::fstream *> drives_;
  int blocks_per_drive_;
  int block_size_;
  int num_disks_;

  // -1 means no failed drive currently
  int failed_drive_ = -1;

  inline void seek_read_(sjtu::fstream *fs, int stripe_index) {
    std::streamoff off = static_cast<std::streamoff>(stripe_index) *
                         static_cast<std::streamoff>(block_size_);
    fs->clear();
    fs->seekg(off, std::ios::beg);
  }
  inline void seek_write_(sjtu::fstream *fs, int stripe_index) {
    std::streamoff off = static_cast<std::streamoff>(stripe_index) *
                         static_cast<std::streamoff>(block_size_);
    fs->clear();
    fs->seekp(off, std::ios::beg);
  }

  // Safe read: fills buf with zeros if read fails/unavailable.
  void read_block_from_drive(int drive_idx, int stripe_index, char *buf) {
    std::fill(buf, buf + block_size_, 0);
    if (drive_idx < 0 || drive_idx >= num_disks_) return;
    if (failed_drive_ == drive_idx) return;
    sjtu::fstream *fs = drives_[drive_idx];
    if (!fs || !fs->is_open()) return;
    seek_read_(fs, stripe_index);
    fs->read(buf, block_size_);
  }

  void write_block_to_drive(int drive_idx, int stripe_index, const char *buf) {
    if (drive_idx < 0 || drive_idx >= num_disks_) return;
    if (failed_drive_ == drive_idx) return;
    sjtu::fstream *fs = drives_[drive_idx];
    if (!fs || !fs->is_open()) return;
    seek_write_(fs, stripe_index);
    fs->write(buf, block_size_);
    fs->flush();
  }

  // XOR into dest: dest ^= src
  inline void xor_buf(char *dest, const char *src) {
    for (int i = 0; i < block_size_; ++i) dest[i] ^= src[i];
  }

  // Mapping helpers for RAID5 (rotating parity)
  inline void map_logical_to_physical(int logical_block, int &stripe, int &data_disk, int &parity_disk) const {
    int data_per_stripe = num_disks_ - 1;
    stripe = logical_block / data_per_stripe;
    int idx_in_stripe = logical_block % data_per_stripe;
    parity_disk = stripe % num_disks_;
    data_disk = (idx_in_stripe < parity_disk) ? idx_in_stripe : (idx_in_stripe + 1);
  }

  // Eager rebuild for a replaced drive (drive_idx).
  void rebuild_drive(int drive_idx) {
    std::vector<char> buf(block_size_);
    std::vector<char> tmp(block_size_);
    for (int s = 0; s < blocks_per_drive_; ++s) {
      int parity_disk = s % num_disks_;
      if (drive_idx == parity_disk) {
        // Rebuild parity as XOR of all data drives for this stripe.
        std::fill(buf.begin(), buf.end(), 0);
        for (int d = 0; d < num_disks_; ++d) {
          if (d == parity_disk) continue;
          read_block_from_drive(d, s, tmp.data());
          xor_buf(buf.data(), tmp.data());
        }
        write_block_to_drive(drive_idx, s, buf.data());
      } else {
        // Rebuild missing data: data = parity ^ XOR(other data)
        std::fill(buf.begin(), buf.end(), 0);
        // Start with parity
        read_block_from_drive(parity_disk, s, buf.data());
        for (int d = 0; d < num_disks_; ++d) {
          if (d == parity_disk || d == drive_idx) continue;
          read_block_from_drive(d, s, tmp.data());
          xor_buf(buf.data(), tmp.data());
        }
        write_block_to_drive(drive_idx, s, buf.data());
      }
    }
  }

public:
  RAID5Controller(std::vector<sjtu::fstream *> drives, int blocks_per_drive, int block_size = 4096)
      : drives_(std::move(drives)), blocks_per_drive_(blocks_per_drive), block_size_(block_size),
        num_disks_(static_cast<int>(drives_.size())) {
    // Files are prepared and opened by caller.
  }

  void Start(EventType event_type_, int drive_id) {
    if (event_type_ == EventType::NORMAL) {
      failed_drive_ = -1;
      return;
    }
    if (drive_id < 0 || drive_id >= num_disks_) return;

    if (event_type_ == EventType::FAILED) {
      // Mark as failed; skip IO to that drive.
      failed_drive_ = drive_id;
      return;
    }

    if (event_type_ == EventType::REPLACED) {
      // Assume the drive has been cleared to zeros and is available.
      // Temporarily assume no other failure.
      failed_drive_ = -1;
      rebuild_drive(drive_id);
      return;
    }
  }

  void Shutdown() {
    for (auto *fs : drives_) {
      if (fs && fs->is_open()) {
        fs->close();
      }
    }
  }

  void ReadBlock(int block_id, char *result) {
    int stripe, data_disk, parity_disk;
    map_logical_to_physical(block_id, stripe, data_disk, parity_disk);

    if (failed_drive_ == data_disk) {
      // Reconstruct missing data block from parity and surviving data blocks.
      std::fill(result, result + block_size_, 0);
      read_block_from_drive(parity_disk, stripe, result);
      std::vector<char> tmp(block_size_);
      for (int d = 0; d < num_disks_; ++d) {
        if (d == parity_disk || d == failed_drive_) continue;
        read_block_from_drive(d, stripe, tmp.data());
        xor_buf(result, tmp.data());
      }
      return;
    }

    // Otherwise read directly (even if parity failed).
    read_block_from_drive(data_disk, stripe, result);
  }

  void WriteBlock(int block_id, const char *data) {
    int stripe, data_disk, parity_disk;
    map_logical_to_physical(block_id, stripe, data_disk, parity_disk);

    if (failed_drive_ < 0) {
      // No failure: compute parity from all data with data_new in place.
      std::vector<char> parity(block_size_, 0);
      std::vector<char> tmp(block_size_);
      for (int d = 0; d < num_disks_; ++d) {
        if (d == parity_disk) continue;
        if (d == data_disk) {
          xor_buf(parity.data(), data);
        } else {
          read_block_from_drive(d, stripe, tmp.data());
          xor_buf(parity.data(), tmp.data());
        }
      }
      write_block_to_drive(data_disk, stripe, data);
      write_block_to_drive(parity_disk, stripe, parity.data());
      return;
    }

    if (failed_drive_ == data_disk) {
      // Update parity to reflect new data; data disk unavailable.
      if (failed_drive_ != parity_disk) {
        std::vector<char> parity(block_size_, 0);
        std::vector<char> tmp(block_size_);
        for (int d = 0; d < num_disks_; ++d) {
          if (d == parity_disk || d == failed_drive_) continue;
          read_block_from_drive(d, stripe, tmp.data());
          xor_buf(parity.data(), tmp.data());
        }
        xor_buf(parity.data(), data);
        write_block_to_drive(parity_disk, stripe, parity.data());
      }
      return;
    }

    if (failed_drive_ == parity_disk) {
      // Parity unavailable: write data only.
      write_block_to_drive(data_disk, stripe, data);
      return;
    }

    // Some other data disk failed: do read-modify-write for parity.
    std::vector<char> old_data(block_size_);
    std::vector<char> old_parity(block_size_);
    read_block_from_drive(data_disk, stripe, old_data.data());
    read_block_from_drive(parity_disk, stripe, old_parity.data());
    for (int i = 0; i < block_size_; ++i) {
      old_parity[i] ^= old_data[i] ^ data[i];
    }
    write_block_to_drive(data_disk, stripe, data);
    write_block_to_drive(parity_disk, stripe, old_parity.data());
  }

  int Capacity() { return (num_disks_ - 1) * blocks_per_drive_; }
};
