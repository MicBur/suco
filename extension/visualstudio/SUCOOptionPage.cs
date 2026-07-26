using System.ComponentModel;
using Microsoft.VisualStudio.Shell;

namespace SUCOGrid.VSIX
{
    public class SUCOOptionPage : DialogPage
    {
        [Category("SUCO Grid")]
        [DisplayName("Coordinator Host")]
        [Description("IP address or hostname of the SUCO Coordinator (default: 192.168.0.200)")]
        public string CoordinatorHost { get; set; } = "192.168.0.200";

        [Category("SUCO Grid")]
        [DisplayName("Coordinator Port")]
        [Description("TCP Port of the SUCO Coordinator (default: 9000)")]
        public int CoordinatorPort { get; set; } = 9000;

        [Category("SUCO Grid")]
        [DisplayName("Enable Launcher Injection")]
        [Description("Automatically inject CMAKE_CXX_COMPILER_LAUNCHER=suco-cl++ into CMake Open Folder settings")]
        public bool EnableLauncherInjection { get; set; } = true;
    }
}
